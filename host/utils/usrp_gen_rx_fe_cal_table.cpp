//
// Copyright 2010 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "usrp_cal_utils.hpp"
#include <uhd/utils/thread_priority.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/paths.hpp>
#include <uhd/utils/algorithm.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <boost/thread/thread.hpp>
#include <boost/math/special_functions/round.hpp>
#include <iostream>
#include <complex>
#include <cmath>
#include <ctime>

namespace po = boost::program_options;

/***********************************************************************
 * Transmit thread
 **********************************************************************/
static void tx_thread(uhd::usrp::multi_usrp::sptr usrp, const double tx_wave_ampl){
    uhd::set_thread_priority_safe();

    //create a transmit streamer
    uhd::stream_args_t stream_args("fc32"); //complex floats
    uhd::tx_streamer::sptr tx_stream = usrp->get_tx_stream(stream_args);

    //setup variables and allocate buffer
    uhd::tx_metadata_t md;
    md.has_time_spec = false;
    std::vector<std::complex<float> > buff(tx_stream->get_max_num_samps()*10);

    //fill buff and send until interrupted
    while (not boost::this_thread::interruption_requested()){
        for (size_t i = 0; i < buff.size(); i++){
            buff[i] = float(tx_wave_ampl);
        }
        tx_stream->send(&buff.front(), buff.size(), md);
    }

    //send a mini EOB packet
    md.end_of_burst = true;
    tx_stream->send("", 0, md);
}

/***********************************************************************
 * Tune RX and TX routine
 **********************************************************************/
static double tune_rx_and_tx(uhd::usrp::multi_usrp::sptr usrp, const double rx_lo_freq, const double tx_offset){
    //tune the receiver with no cordic
    uhd::tune_request_t rx_tune_req(rx_lo_freq);
    rx_tune_req.dsp_freq_policy = uhd::tune_request_t::POLICY_MANUAL;
    rx_tune_req.dsp_freq = 0;
    usrp->set_rx_freq(rx_tune_req);

    //tune the transmitter with no cordic
    uhd::tune_request_t tx_tune_req(usrp->get_rx_freq() - tx_offset);
    tx_tune_req.dsp_freq_policy = uhd::tune_request_t::POLICY_MANUAL;
    tx_tune_req.dsp_freq = 0;
    usrp->set_tx_freq(tx_tune_req);

    //wait for the LOs to become locked
    boost::this_thread::sleep(boost::posix_time::milliseconds(50));
    boost::system_time start = boost::get_system_time();
    while (not usrp->get_tx_sensor("lo_locked").to_bool() or not usrp->get_rx_sensor("lo_locked").to_bool()){
        if (boost::get_system_time() > start + boost::posix_time::milliseconds(100)){
            throw std::runtime_error("timed out waiting for TX and/or RX LO to lock");
        }
    }

    return usrp->get_rx_freq();
}

/***********************************************************************
 * Data capture routine
 **********************************************************************/
static void capture_samples(uhd::usrp::multi_usrp::sptr usrp, uhd::rx_streamer::sptr rx_stream, std::vector<std::complex<float> > &buff){
    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    stream_cmd.num_samps = buff.size();
    stream_cmd.stream_now = true;
    usrp->issue_stream_cmd(stream_cmd);
    uhd::rx_metadata_t md;
    const size_t num_rx_samps = rx_stream->recv(&buff.front(), buff.size(), md);

    //validate the received data
    if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE){
        throw std::runtime_error(str(boost::format(
            "Unexpected error code 0x%x"
        ) % md.error_code));
    }
    if (num_rx_samps != buff.size()){
        throw std::runtime_error("did not get all the samples requested");
    }
}

/***********************************************************************
 * Main
 **********************************************************************/
int UHD_SAFE_MAIN(int argc, char *argv[]){
    std::string args;
    double rate, tx_wave_ampl, tx_offset, freq_step, tx_gain, rx_gain;
    size_t nsamps;

    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "help message")
        ("verbose", "enable some verbose")
        ("args", po::value<std::string>(&args)->default_value(""), "device address args [default = \"\"]")
        ("rate", po::value<double>(&rate)->default_value(12.5e6), "RX and TX sample rate in Hz")
        ("tx_wave_ampl", po::value<double>(&tx_wave_ampl)->default_value(0.7), "Transmit wave amplitude in counts")
        ("tx_offset", po::value<double>(&tx_offset)->default_value(.9344e6), "TX LO offset from the RX LO in Hz")
        ("tx_gain", po::value<double>(&tx_gain)->default_value(0), "TX gain in dB")
        ("rx_gain", po::value<double>(&rx_gain)->default_value(0), "RX gain in dB")
        ("freq_step", po::value<double>(&freq_step)->default_value(10e6), "Step size for LO sweep in Hz")
        ("nsamps", po::value<size_t>(&nsamps)->default_value(10000), "Samples per data capture")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    //print the help message
    if (vm.count("help")){
        std::cout << boost::format("USRP Generate RX Frontend Calibration Table %s") % desc << std::endl;
        std::cout <<
            "This application measures leakage between RX and TX on an XCVR daughterboard to self-calibrate.\n"
            << std::endl;
        return ~0;
    }

    //create a usrp device
    std::cout << std::endl;
    std::cout << boost::format("Creating the usrp device with: %s...") % args << std::endl;
    uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(args);

    //set the antennas to cal
    if (not uhd::has(usrp->get_rx_antennas(), "CAL") or not uhd::has(usrp->get_tx_antennas(), "CAL")){
        throw std::runtime_error("This board does not have the CAL antenna option, cannot self-calibrate.");
    }
    usrp->set_rx_antenna("CAL");
    usrp->set_tx_antenna("CAL");

    //set the sample rates
    usrp->set_rx_rate(rate);
    usrp->set_tx_rate(rate);

    //set midrange rx gain, default 0 tx gain
    usrp->set_tx_gain(tx_gain);
    usrp->set_rx_gain(rx_gain);

    //create a receive streamer
    uhd::stream_args_t stream_args("fc32"); //complex floats
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

    //create a transmitter thread
    boost::thread_group threads;
    threads.create_thread(boost::bind(&tx_thread, usrp, tx_wave_ampl));

    //re-usable buffer for samples
    std::vector<std::complex<float> > buff(nsamps);

    //store the results here
    std::vector<result_t> results;

    const uhd::meta_range_t freq_range = usrp->get_rx_freq_range();
    for (double rx_lo_i = freq_range.start()+50e6; rx_lo_i < freq_range.stop()-50e6; rx_lo_i += freq_step){
        const double rx_lo = tune_rx_and_tx(usrp, rx_lo_i, tx_offset);

        //bounds and results from searching
        std::complex<double> best_correction;
        double phase_corr_start = -.3, phase_corr_stop = .3, phase_corr_step;
        double ampl_corr_start = -.3, ampl_corr_stop = .3, ampl_corr_step;
        double best_suppression = 0, best_phase_corr = 0, best_ampl_corr = 0;

        for (size_t i = 0; i < num_search_iters; i++){

            phase_corr_step = (phase_corr_stop - phase_corr_start)/(num_search_steps-1);
            ampl_corr_step = (ampl_corr_stop - ampl_corr_start)/(num_search_steps-1);

            for (double phase_corr = phase_corr_start; phase_corr <= phase_corr_stop + phase_corr_step/2; phase_corr += phase_corr_step){
            for (double ampl_corr = ampl_corr_start; ampl_corr <= ampl_corr_stop + ampl_corr_step/2; ampl_corr += ampl_corr_step){

                const std::complex<double> correction = std::polar(ampl_corr+1, phase_corr*tau);
                usrp->set_rx_iq_balance(correction);

                //receive some samples
                capture_samples(usrp, rx_stream, buff);

                const double actual_rx_rate = usrp->get_rx_rate();
                const double actual_tx_freq = usrp->get_tx_freq();
                const double actual_rx_freq = usrp->get_rx_freq();
                const double bb_tone_freq = actual_tx_freq - actual_rx_freq;
                const double bb_imag_freq = -bb_tone_freq;

                const double tone_dbrms = compute_tone_dbrms(buff, bb_tone_freq/actual_rx_rate);
                const double imag_dbrms = compute_tone_dbrms(buff, bb_imag_freq/actual_rx_rate);
                const double suppression = tone_dbrms - imag_dbrms;

                //std::cout << "bb_tone_freq " << bb_tone_freq << std::endl;
                //std::cout << "bb_imag_freq " << bb_imag_freq << std::endl;
                //std::cout << "tone_dbrms " << tone_dbrms << std::endl;
                //std::cout << "imag_dbrms " << imag_dbrms << std::endl;
                //std::cout << "suppression " << (tone_dbrms - imag_dbrms) << std::endl;

                if (suppression > best_suppression){
                    best_correction = correction;
                    best_suppression = suppression;
                    best_phase_corr = phase_corr;
                    best_ampl_corr = ampl_corr;
                }

            }}

            //std::cout << "best_phase_corr " << best_phase_corr << std::endl;
            //std::cout << "best_ampl_corr " << best_ampl_corr << std::endl;
            //std::cout << "best_suppression " << best_suppression << std::endl;

            phase_corr_start = best_phase_corr - phase_corr_step;
            phase_corr_stop = best_phase_corr + phase_corr_step;
            ampl_corr_start = best_ampl_corr - ampl_corr_step;
            ampl_corr_stop = best_ampl_corr + ampl_corr_step;
        }

        if (best_suppression > 30){ //most likely valid, keep result
            result_t result;
            result.freq = rx_lo;
            result.real_corr = best_correction.real();
            result.imag_corr = best_correction.imag();
            result.sup = best_suppression;
            results.push_back(result);
        }
        if (vm.count("verbose")){
            std::cout << boost::format("%f MHz: best suppression %fdB") % (rx_lo/1e6) % best_suppression << std::endl;
        }
        else std::cout << "." << std::flush;

    }
    std::cout << std::endl;

    //stop the transmitter
    threads.interrupt_all();
    threads.join_all();

    store_results(usrp, results, "RX", "rx");

    return 0;
}
