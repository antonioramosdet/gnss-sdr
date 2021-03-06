/*!
 * \file galileo_e1_dll_pll_veml_tracking.cc
 * \brief  Adapts a DLL+PLL VEML (Very Early Minus Late) tracking loop block
 *   to a TrackingInterface for Galileo E1 signals
 * \author Luis Esteve, 2012. luis(at)epsilon-formacion.com
 *
 * Code DLL + carrier PLL according to the algorithms described in:
 * K.Borre, D.M.Akos, N.Bertelsen, P.Rinder, and S.H.Jensen,
 * A Software-Defined GPS and Galileo Receiver. A Single-Frequency
 * Approach, Birkhauser, 2007
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2018  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * GNSS-SDR is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GNSS-SDR is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNSS-SDR. If not, see <https://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------
 */

#include "dll_pll_conf.h"
#include "galileo_e1_dll_pll_veml_tracking.h"
#include "configuration_interface.h"
#include "Galileo_E1.h"
#include "gnss_sdr_flags.h"
#include "display.h"
#include <glog/logging.h>


using google::LogMessage;

GalileoE1DllPllVemlTracking::GalileoE1DllPllVemlTracking(
    ConfigurationInterface* configuration, std::string role,
    unsigned int in_streams, unsigned int out_streams) : role_(role), in_streams_(in_streams), out_streams_(out_streams)
{
    Dll_Pll_Conf trk_param = Dll_Pll_Conf();
    DLOG(INFO) << "role " << role;
    //################# CONFIGURATION PARAMETERS ########################
    std::string default_item_type = "gr_complex";
    std::string item_type = configuration->property(role + ".item_type", default_item_type);
    int fs_in_deprecated = configuration->property("GNSS-SDR.internal_fs_hz", 2048000);
    int fs_in = configuration->property("GNSS-SDR.internal_fs_sps", fs_in_deprecated);
    trk_param.fs_in = fs_in;
    bool dump = configuration->property(role + ".dump", false);
    trk_param.dump = dump;
    float pll_bw_hz = configuration->property(role + ".pll_bw_hz", 5.0);
    if (FLAGS_pll_bw_hz != 0.0) pll_bw_hz = static_cast<float>(FLAGS_pll_bw_hz);
    trk_param.pll_bw_hz = pll_bw_hz;
    float dll_bw_hz = configuration->property(role + ".dll_bw_hz", 0.5);
    if (FLAGS_dll_bw_hz != 0.0) dll_bw_hz = static_cast<float>(FLAGS_dll_bw_hz);
    trk_param.dll_bw_hz = dll_bw_hz;
    float pll_bw_narrow_hz = configuration->property(role + ".pll_bw_narrow_hz", 2.0);
    trk_param.pll_bw_narrow_hz = pll_bw_narrow_hz;
    float dll_bw_narrow_hz = configuration->property(role + ".dll_bw_narrow_hz", 0.25);
    trk_param.dll_bw_narrow_hz = dll_bw_narrow_hz;
    int extend_correlation_symbols = configuration->property(role + ".extend_correlation_symbols", 1);
    float early_late_space_chips = configuration->property(role + ".early_late_space_chips", 0.15);
    trk_param.early_late_space_chips = early_late_space_chips;
    float very_early_late_space_chips = configuration->property(role + ".very_early_late_space_chips", 0.6);
    trk_param.very_early_late_space_chips = very_early_late_space_chips;
    float early_late_space_narrow_chips = configuration->property(role + ".early_late_space_narrow_chips", 0.15);
    trk_param.early_late_space_narrow_chips = early_late_space_narrow_chips;
    float very_early_late_space_narrow_chips = configuration->property(role + ".very_early_late_space_narrow_chips", 0.6);
    trk_param.very_early_late_space_narrow_chips = very_early_late_space_narrow_chips;
    bool track_pilot = configuration->property(role + ".track_pilot", false);
    if (extend_correlation_symbols < 1)
        {
            extend_correlation_symbols = 1;
            std::cout << TEXT_RED << "WARNING: Galileo E1. extend_correlation_symbols must be bigger than 0. Coherent integration has been set to 1 symbol (4 ms)" << TEXT_RESET << std::endl;
        }
    else if (!track_pilot and extend_correlation_symbols > 1)
        {
            extend_correlation_symbols = 1;
            std::cout << TEXT_RED << "WARNING: Galileo E1. Extended coherent integration is not allowed when tracking the data component. Coherent integration has been set to 4 ms (1 symbol)" << TEXT_RESET << std::endl;
        }
    if ((extend_correlation_symbols > 1) and (pll_bw_narrow_hz > pll_bw_hz or dll_bw_narrow_hz > dll_bw_hz))
        {
            std::cout << TEXT_RED << "WARNING: Galileo E1. PLL or DLL narrow tracking bandwidth is higher than wide tracking one" << TEXT_RESET << std::endl;
        }
    trk_param.track_pilot = track_pilot;
    trk_param.extend_correlation_symbols = extend_correlation_symbols;
    std::string default_dump_filename = "./track_ch";
    std::string dump_filename = configuration->property(role + ".dump_filename", default_dump_filename);
    trk_param.dump_filename = dump_filename;
    int vector_length = std::round(fs_in / (Galileo_E1_CODE_CHIP_RATE_HZ / Galileo_E1_B_CODE_LENGTH_CHIPS));
    trk_param.vector_length = vector_length;
    trk_param.system = 'E';
    char sig_[3] = "1B";
    std::memcpy(trk_param.signal, sig_, 3);
    int cn0_samples = configuration->property(role + ".cn0_samples", 20);
    if (FLAGS_cn0_samples != 20) cn0_samples = FLAGS_cn0_samples;
    trk_param.cn0_samples = cn0_samples;
    int cn0_min = configuration->property(role + ".cn0_min", 25);
    if (FLAGS_cn0_min != 25) cn0_min = FLAGS_cn0_min;
    trk_param.cn0_min = cn0_min;
    int max_lock_fail = configuration->property(role + ".max_lock_fail", 50);
    if (FLAGS_max_lock_fail != 50) max_lock_fail = FLAGS_max_lock_fail;
    trk_param.max_lock_fail = max_lock_fail;
    double carrier_lock_th = configuration->property(role + ".carrier_lock_th", 0.85);
    if (FLAGS_carrier_lock_th != 0.85) carrier_lock_th = FLAGS_carrier_lock_th;
    trk_param.carrier_lock_th = carrier_lock_th;

    //################# MAKE TRACKING GNURadio object ###################
    if (item_type.compare("gr_complex") == 0)
        {
            item_size_ = sizeof(gr_complex);
            tracking_ = dll_pll_veml_make_tracking(trk_param);
        }
    else
        {
            item_size_ = sizeof(gr_complex);
            LOG(WARNING) << item_type << " unknown tracking item type.";
        }

    channel_ = 0;
    DLOG(INFO) << "tracking(" << tracking_->unique_id() << ")";
    if (in_streams_ > 1)
        {
            LOG(ERROR) << "This implementation only supports one input stream";
        }
    if (out_streams_ > 1)
        {
            LOG(ERROR) << "This implementation only supports one output stream";
        }
}


GalileoE1DllPllVemlTracking::~GalileoE1DllPllVemlTracking()
{
}


void GalileoE1DllPllVemlTracking::start_tracking()
{
    tracking_->start_tracking();
}


/*
 * Set tracking channel unique ID
 */
void GalileoE1DllPllVemlTracking::set_channel(unsigned int channel)
{
    channel_ = channel;
    tracking_->set_channel(channel);
}


void GalileoE1DllPllVemlTracking::set_gnss_synchro(Gnss_Synchro* p_gnss_synchro)
{
    tracking_->set_gnss_synchro(p_gnss_synchro);
}


void GalileoE1DllPllVemlTracking::connect(gr::top_block_sptr top_block)
{
    if (top_block)
        { /* top_block is not null */
        };
    //nothing to connect, now the tracking uses gr_sync_decimator
}


void GalileoE1DllPllVemlTracking::disconnect(gr::top_block_sptr top_block)
{
    if (top_block)
        { /* top_block is not null */
        };
    //nothing to disconnect, now the tracking uses gr_sync_decimator
}


gr::basic_block_sptr GalileoE1DllPllVemlTracking::get_left_block()
{
    return tracking_;
}


gr::basic_block_sptr GalileoE1DllPllVemlTracking::get_right_block()
{
    return tracking_;
}
