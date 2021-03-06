/*!
 * \file control_thread.cc
 * \brief This class implements the receiver control plane
 * \author Carlos Aviles, 2010. carlos.avilesr(at)googlemail.com
 *
 * GNSS Receiver Control Plane: connects the flowgraph, starts running it,
 * and while it does not stop, reads the control messages generated by the blocks,
 * process them, and apply the corresponding actions.
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

#include "control_thread.h"
#include "concurrent_queue.h"
#include "concurrent_map.h"
#include "control_message_factory.h"
#include "file_configuration.h"
#include "gnss_flowgraph.h"
#include "gnss_sdr_flags.h"
#include "galileo_ephemeris.h"
#include "galileo_iono.h"
#include "galileo_utc_model.h"
#include "galileo_almanac.h"
#include "gps_ephemeris.h"
#include "gps_iono.h"
#include "gps_utc_model.h"
#include "gps_almanac.h"
#include <boost/lexical_cast.hpp>
#include <boost/chrono.hpp>
#include <glog/logging.h>
#include <gnuradio/message.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <string>


extern concurrent_map<Gps_Acq_Assist> global_gps_acq_assist_map;
extern concurrent_queue<Gps_Acq_Assist> global_gps_acq_assist_queue;

using google::LogMessage;


ControlThread::ControlThread()
{
    if (!FLAGS_c.compare("-"))
        {
            configuration_ = std::make_shared<FileConfiguration>(FLAGS_config_file);
        }
    else
        {
            configuration_ = std::make_shared<FileConfiguration>(FLAGS_c);
        }
    delete_configuration_ = false;
    init();
}


ControlThread::ControlThread(std::shared_ptr<ConfigurationInterface> configuration)
{
    configuration_ = configuration;
    delete_configuration_ = false;
    init();
}


ControlThread::~ControlThread()
{
    // save navigation data to files
    // if (save_assistance_to_XML() == true) {}
    if (msqid != -1) msgctl(msqid, IPC_RMID, NULL);
}


/*
 * Runs the control thread that manages the receiver control plane
 *
 * This is the main loop that reads and process the control messages
 * 1- Connect the GNSS receiver flowgraph
 * 2- Start the GNSS receiver flowgraph
 *    while (flowgraph_->running() && !stop)_{
 * 3- Read control messages and process them }
 */
void ControlThread::run()
{
    // Connect the flowgraph
    try
        {
            flowgraph_->connect();
        }
    catch (const std::exception &e)
        {
            LOG(ERROR) << e.what();
            return;
        }
    if (flowgraph_->connected())
        {
            LOG(INFO) << "Flowgraph connected";
        }
    else
        {
            LOG(ERROR) << "Unable to connect flowgraph";
            return;
        }
    // Start the flowgraph
    flowgraph_->start();
    if (flowgraph_->running())
        {
            LOG(INFO) << "Flowgraph started";
        }
    else
        {
            LOG(ERROR) << "Unable to start flowgraph";
            return;
        }

    //launch GNSS assistance process AFTER the flowgraph is running because the GNURadio asynchronous queues must be already running to transport msgs
    assist_GNSS();
    // start the keyboard_listener thread
    keyboard_thread_ = boost::thread(&ControlThread::keyboard_listener, this);
    sysv_queue_thread_ = boost::thread(&ControlThread::sysv_queue_listener, this);

    bool enable_FPGA = configuration_->property("Channel.enable_FPGA", false);

    if (enable_FPGA == true)
        {
            flowgraph_->start_acquisition_helper();
        }

    // Main loop to read and process the control messages
    while (flowgraph_->running() && !stop_)
        {
            //TODO re-enable the blocking read messages functions and fork the process
            read_control_messages();
            if (control_messages_ != 0) process_control_messages();
        }
    std::cout << "Stopping GNSS-SDR, please wait!" << std::endl;
    flowgraph_->stop();
    stop_ = true;
    flowgraph_->disconnect();

    //Join keyboard thread
#ifdef OLD_BOOST
    keyboard_thread_.timed_join(boost::posix_time::seconds(1));
    sysv_queue_thread_.timed_join(boost::posix_time::seconds(1));
#endif
#ifndef OLD_BOOST
    keyboard_thread_.try_join_until(boost::chrono::steady_clock::now() + boost::chrono::milliseconds(1000));
    sysv_queue_thread_.try_join_until(boost::chrono::steady_clock::now() + boost::chrono::milliseconds(1000));
#endif

    LOG(INFO) << "Flowgraph stopped";
}


void ControlThread::set_control_queue(gr::msg_queue::sptr control_queue)
{
    if (flowgraph_->running())
        {
            LOG(WARNING) << "Unable to set control queue while flowgraph is running";
            return;
        }
    control_queue_ = control_queue;
}


/*
 * Returns true if reading was successful
 */
bool ControlThread::read_assistance_from_XML()
{
    // return variable (true == succeeded)
    bool ret = false;
    // getting names from the config file, if available
    std::string eph_xml_filename = configuration_->property("GNSS-SDR.SUPL_gps_ephemeris_xml", eph_default_xml_filename);
    std::string utc_xml_filename = configuration_->property("GNSS-SDR.SUPL_gps_utc_model.xml", utc_default_xml_filename);
    std::string iono_xml_filename = configuration_->property("GNSS-SDR.SUPL_gps_iono_xml", iono_default_xml_filename);
    std::string ref_time_xml_filename = configuration_->property("GNSS-SDR.SUPL_gps_ref_time_xml", ref_time_default_xml_filename);
    std::string ref_location_xml_filename = configuration_->property("GNSS-SDR.SUPL_gps_ref_location_xml", ref_location_default_xml_filename);

    std::cout << "SUPL: Try read GPS ephemeris from XML file " << eph_xml_filename << std::endl;
    if (supl_client_ephemeris_.load_ephemeris_xml(eph_xml_filename) == true)
        {
            std::map<int, Gps_Ephemeris>::const_iterator gps_eph_iter;
            for (gps_eph_iter = supl_client_ephemeris_.gps_ephemeris_map.cbegin();
                 gps_eph_iter != supl_client_ephemeris_.gps_ephemeris_map.cend();
                 gps_eph_iter++)
                {
                    std::cout << "SUPL: Read XML Ephemeris for GPS SV " << gps_eph_iter->first << std::endl;
                    std::shared_ptr<Gps_Ephemeris> tmp_obj = std::make_shared<Gps_Ephemeris>(gps_eph_iter->second);
                    flowgraph_->send_telemetry_msg(pmt::make_any(tmp_obj));
                }
            ret = true;
        }
    else
        {
            std::cout << "ERROR: SUPL client error reading XML" << std::endl;
            std::cout << "Disabling SUPL assistance..." << std::endl;
        }
    // Only look for {utc, iono, ref time, ref location} if SUPL is enabled
    bool enable_gps_supl_assistance = configuration_->property("GNSS-SDR.SUPL_gps_enabled", false);
    if (enable_gps_supl_assistance == true)
        {
            // Try to read UTC model from XML
            if (supl_client_acquisition_.load_utc_xml(utc_xml_filename) == true)
                {
                    LOG(INFO) << "SUPL: Read XML UTC model";
                    std::shared_ptr<Gps_Utc_Model> tmp_obj = std::make_shared<Gps_Utc_Model>(supl_client_acquisition_.gps_utc);
                    flowgraph_->send_telemetry_msg(pmt::make_any(tmp_obj));
                }
            else
                {
                    LOG(INFO) << "SUPL: couldn't read UTC model XML";
                }

            // Try to read Iono model from XML
            if (supl_client_acquisition_.load_iono_xml(iono_xml_filename) == true)
                {
                    LOG(INFO) << "SUPL: Read XML IONO model";
                    std::shared_ptr<Gps_Iono> tmp_obj = std::make_shared<Gps_Iono>(supl_client_acquisition_.gps_iono);
                    flowgraph_->send_telemetry_msg(pmt::make_any(tmp_obj));
                }
            else
                {
                    LOG(INFO) << "SUPL: couldn't read IONO model XML";
                }

            // Try to read Ref Time from XML
            if (supl_client_acquisition_.load_ref_time_xml(ref_time_xml_filename) == true)
                {
                    LOG(INFO) << "SUPL: Read XML Ref Time";
                    std::shared_ptr<Gps_Ref_Time> tmp_obj = std::make_shared<Gps_Ref_Time>(supl_client_acquisition_.gps_time);
                    flowgraph_->send_telemetry_msg(pmt::make_any(tmp_obj));
                }
            else
                {
                    LOG(INFO) << "SUPL: couldn't read Ref Time XML";
                }

            // Try to read Ref Location from XML
            if (supl_client_acquisition_.load_ref_location_xml(ref_location_xml_filename) == true)
                {
                    LOG(INFO) << "SUPL: Read XML Ref Location";
                    std::shared_ptr<Gps_Ref_Location> tmp_obj = std::make_shared<Gps_Ref_Location>(supl_client_acquisition_.gps_ref_loc);
                    flowgraph_->send_telemetry_msg(pmt::make_any(tmp_obj));
                }
            else
                {
                    LOG(INFO) << "SUPL: couldn't read Ref Location XML";
                }
        }

    return ret;
}


void ControlThread::assist_GNSS()
{
    //######### GNSS Assistance #################################
    // GNSS Assistance configuration
    bool enable_gps_supl_assistance = configuration_->property("GNSS-SDR.SUPL_gps_enabled", false);
    if (enable_gps_supl_assistance == true)
        //SUPL SERVER TEST. Not operational yet!
        {
            std::cout << "SUPL RRLP GPS assistance enabled!" << std::endl;
            std::string default_acq_server = "supl.nokia.com";
            std::string default_eph_server = "supl.google.com";
            supl_client_ephemeris_.server_name = configuration_->property("GNSS-SDR.SUPL_gps_ephemeris_server", default_acq_server);
            supl_client_acquisition_.server_name = configuration_->property("GNSS-SDR.SUPL_gps_acquisition_server", default_eph_server);
            supl_client_ephemeris_.server_port = configuration_->property("GNSS-SDR.SUPL_gps_ephemeris_port", 7275);
            supl_client_acquisition_.server_port = configuration_->property("GNSS-SDR.SUPL_gps_acquisition_port", 7275);
            supl_mcc = configuration_->property("GNSS-SDR.SUPL_MCC", 244);
            supl_mns = configuration_->property("GNSS-SDR.SUPL_MNS", 5);

            std::string default_lac = "0x59e2";
            std::string default_ci = "0x31b0";
            try
                {
                    supl_lac = boost::lexical_cast<int>(configuration_->property("GNSS-SDR.SUPL_LAC", default_lac));
                }
            catch (boost::bad_lexical_cast &)
                {
                    supl_lac = 0x59e2;
                }

            try
                {
                    supl_ci = boost::lexical_cast<int>(configuration_->property("GNSS-SDR.SUPL_CI", default_ci));
                }
            catch (boost::bad_lexical_cast &)
                {
                    supl_ci = 0x31b0;
                }

            bool SUPL_read_gps_assistance_xml = configuration_->property("GNSS-SDR.SUPL_read_gps_assistance_xml", false);
            if (SUPL_read_gps_assistance_xml == true)
                {
                    // read assistance from file
                    if (read_assistance_from_XML())
                        {
                            std::cout << "GPS assistance data loaded from local XML file." << std::endl;
                        }
                }
            else
                {
                    // Request ephemeris from SUPL server
                    int error;
                    supl_client_ephemeris_.request = 1;
                    std::cout << "SUPL: Try to read GPS ephemeris from SUPL server..." << std::endl;
                    error = supl_client_ephemeris_.get_assistance(supl_mcc, supl_mns, supl_lac, supl_ci);
                    if (error == 0)
                        {
                            std::map<int, Gps_Ephemeris>::const_iterator gps_eph_iter;
                            for (gps_eph_iter = supl_client_ephemeris_.gps_ephemeris_map.cbegin();
                                 gps_eph_iter != supl_client_ephemeris_.gps_ephemeris_map.cend();
                                 gps_eph_iter++)
                                {
                                    std::cout << "SUPL: Received Ephemeris for GPS SV " << gps_eph_iter->first << std::endl;
                                    std::shared_ptr<Gps_Ephemeris> tmp_obj = std::make_shared<Gps_Ephemeris>(gps_eph_iter->second);
                                    flowgraph_->send_telemetry_msg(pmt::make_any(tmp_obj));
                                }
                            //Save ephemeris to XML file
                            std::string eph_xml_filename = configuration_->property("GNSS-SDR.SUPL_gps_ephemeris_xml", eph_default_xml_filename);
                            if (supl_client_ephemeris_.save_ephemeris_map_xml(eph_xml_filename, supl_client_ephemeris_.gps_ephemeris_map) == true)
                                {
                                    std::cout << "SUPL: XML Ephemeris file created" << std::endl;
                                }
                            else
                                {
                                    std::cout << "SUPL: Failed to create XML Ephemeris file" << std::endl;
                                }
                        }
                    else
                        {
                            std::cout << "ERROR: SUPL client for Ephemeris returned " << error << std::endl;
                            std::cout << "Please check internet connection and SUPL server configuration" << error << std::endl;
                            std::cout << "Trying to read ephemeris from XML file" << std::endl;
                            if (read_assistance_from_XML() == false)
                                {
                                    std::cout << "ERROR: Could not read Ephemeris file: Disabling SUPL assistance." << std::endl;
                                }
                        }

                    // Request almanac , IONO and UTC Model
                    supl_client_ephemeris_.request = 0;
                    std::cout << "SUPL: Try read Almanac, Iono, Utc Model, Ref Time and Ref Location from SUPL server..." << std::endl;
                    error = supl_client_ephemeris_.get_assistance(supl_mcc, supl_mns, supl_lac, supl_ci);
                    if (error == 0)
                        {
                            std::map<int, Gps_Almanac>::const_iterator gps_alm_iter;
                            for (gps_alm_iter = supl_client_ephemeris_.gps_almanac_map.cbegin();
                                 gps_alm_iter != supl_client_ephemeris_.gps_almanac_map.cend();
                                 gps_alm_iter++)
                                {
                                    std::cout << "SUPL: Received Almanac for GPS SV " << gps_alm_iter->first << std::endl;
                                    std::shared_ptr<Gps_Almanac> tmp_obj = std::make_shared<Gps_Almanac>(gps_alm_iter->second);
                                    flowgraph_->send_telemetry_msg(pmt::make_any(tmp_obj));
                                }
                            if (supl_client_ephemeris_.gps_iono.valid == true)
                                {
                                    std::cout << "SUPL: Received GPS Iono" << std::endl;
                                    std::shared_ptr<Gps_Iono> tmp_obj = std::make_shared<Gps_Iono>(supl_client_ephemeris_.gps_iono);
                                    flowgraph_->send_telemetry_msg(pmt::make_any(tmp_obj));
                                }
                            if (supl_client_ephemeris_.gps_utc.valid == true)
                                {
                                    std::cout << "SUPL: Received GPS UTC Model" << std::endl;
                                    std::shared_ptr<Gps_Utc_Model> tmp_obj = std::make_shared<Gps_Utc_Model>(supl_client_ephemeris_.gps_utc);
                                    flowgraph_->send_telemetry_msg(pmt::make_any(tmp_obj));
                                }
                        }
                    else
                        {
                            std::cout << "ERROR: SUPL client for Almanac returned " << error << std::endl;
                            std::cout << "Please check internet connection and SUPL server configuration" << error << std::endl;
                            std::cout << "Disabling SUPL assistance." << std::endl;
                        }

                    // Request acquisition assistance
                    supl_client_acquisition_.request = 2;
                    std::cout << "SUPL: Try read Acquisition assistance from SUPL server..." << std::endl;
                    error = supl_client_acquisition_.get_assistance(supl_mcc, supl_mns, supl_lac, supl_ci);
                    if (error == 0)
                        {
                            std::map<int, Gps_Acq_Assist>::const_iterator gps_acq_iter;
                            for (gps_acq_iter = supl_client_acquisition_.gps_acq_map.cbegin();
                                 gps_acq_iter != supl_client_acquisition_.gps_acq_map.cend();
                                 gps_acq_iter++)
                                {
                                    std::cout << "SUPL: Received Acquisition assistance for GPS SV " << gps_acq_iter->first << std::endl;
                                    global_gps_acq_assist_map.write(gps_acq_iter->second.i_satellite_PRN, gps_acq_iter->second);
                                }
                            if (supl_client_acquisition_.gps_ref_loc.valid == true)
                                {
                                    std::cout << "SUPL: Received Ref Location (Acquisition Assistance)" << std::endl;
                                    std::shared_ptr<Gps_Ref_Location> tmp_obj = std::make_shared<Gps_Ref_Location>(supl_client_acquisition_.gps_ref_loc);
                                    flowgraph_->send_telemetry_msg(pmt::make_any(tmp_obj));
                                }
                            if (supl_client_acquisition_.gps_time.valid == true)
                                {
                                    std::cout << "SUPL: Received Ref Time (Acquisition Assistance)" << std::endl;
                                    std::shared_ptr<Gps_Ref_Time> tmp_obj = std::make_shared<Gps_Ref_Time>(supl_client_acquisition_.gps_time);
                                    flowgraph_->send_telemetry_msg(pmt::make_any(tmp_obj));
                                }
                        }
                    else
                        {
                            std::cout << "ERROR: SUPL client for Acquisition assistance returned " << error << std::endl;
                            std::cout << "Please check internet connection and SUPL server configuration" << error << std::endl;
                            std::cout << "Disabling SUPL assistance.." << std::endl;
                        }
                }
        }
}


void ControlThread::init()
{
    // Instantiates a control queue, a GNSS flowgraph, and a control message factory
    control_queue_ = gr::msg_queue::make(0);
    try
        {
            flowgraph_ = std::make_shared<GNSSFlowgraph>(configuration_, control_queue_);
        }
    catch (const boost::bad_lexical_cast &e)
        {
            std::cout << "Caught bad lexical cast with error " << e.what() << std::endl;
        }
    control_message_factory_ = std::make_shared<ControlMessageFactory>();
    stop_ = false;
    processed_control_messages_ = 0;
    applied_actions_ = 0;
    supl_mcc = 0;
    supl_mns = 0;
    supl_lac = 0;
    supl_ci = 0;
    msqid = -1;
}


void ControlThread::read_control_messages()
{
    DLOG(INFO) << "Reading control messages from queue";
    gr::message::sptr queue_message = control_queue_->delete_head();
    if (queue_message != 0)
        {
            control_messages_ = control_message_factory_->GetControlMessages(queue_message);
        }
    else
        {
            control_messages_->clear();
        }
}


// Apply the corresponding control actions
// TODO:  May be it is better to move the apply_action state machine to the control_thread
void ControlThread::process_control_messages()
{
    for (unsigned int i = 0; i < control_messages_->size(); i++)
        {
            if (stop_) break;
            if (control_messages_->at(i)->who == 200)
                {
                    apply_action(control_messages_->at(i)->what);
                }
            else
                {
                    flowgraph_->apply_action(control_messages_->at(i)->who, control_messages_->at(i)->what);
                }
            processed_control_messages_++;
        }
    control_messages_->clear();
    DLOG(INFO) << "Processed all control messages";
}


void ControlThread::apply_action(unsigned int what)
{
    switch (what)
        {
        case 0:
            DLOG(INFO) << "Received action STOP";
            stop_ = true;
            applied_actions_++;
            break;
        default:
            DLOG(INFO) << "Unrecognized action.";
            break;
        }
}


void ControlThread::gps_acq_assist_data_collector()
{
    // ############ 1.bis READ EPHEMERIS/UTC_MODE/IONO QUEUE ####################
    Gps_Acq_Assist gps_acq;
    Gps_Acq_Assist gps_acq_old;
    while (stop_ == false)
        {
            global_gps_acq_assist_queue.wait_and_pop(gps_acq);
            if (gps_acq.i_satellite_PRN == 0) break;

            // DEBUG MESSAGE
            std::cout << "Acquisition assistance record has arrived from SAT ID "
                      << gps_acq.i_satellite_PRN
                      << " with Doppler "
                      << gps_acq.d_Doppler0
                      << " [Hz] " << std::endl;
            // insert new acq record to the global ephemeris map
            if (global_gps_acq_assist_map.read(gps_acq.i_satellite_PRN, gps_acq_old))
                {
                    std::cout << "Acquisition assistance record updated" << std::endl;
                    global_gps_acq_assist_map.write(gps_acq.i_satellite_PRN, gps_acq);
                }
            else
                {
                    // insert new acq record
                    LOG(INFO) << "New acq assist record inserted";
                    global_gps_acq_assist_map.write(gps_acq.i_satellite_PRN, gps_acq);
                }
        }
}


void ControlThread::sysv_queue_listener()
{
    typedef struct
    {
        long mtype;  // required by SysV queue messaging
        double stop_message;
    } stop_msgbuf;

    bool read_queue = true;
    stop_msgbuf msg;
    double received_message = 0.0;
    int msgrcv_size = sizeof(msg.stop_message);

    key_t key = 1102;

    if ((msqid = msgget(key, 0644 | IPC_CREAT)) == -1)
        {
            perror("GNSS-SDR cannot create SysV message queues");
            exit(1);
        }

    while (read_queue && !stop_)
        {
            if (msgrcv(msqid, &msg, msgrcv_size, 1, 0) != -1)
                {
                    received_message = msg.stop_message;
                    if ((std::abs(received_message - (-200.0)) < 10 * std::numeric_limits<double>::epsilon()))
                        {
                            std::cout << "Quit order received, stopping GNSS-SDR !!" << std::endl;
                            std::unique_ptr<ControlMessageFactory> cmf(new ControlMessageFactory());
                            if (control_queue_ != gr::msg_queue::sptr())
                                {
                                    control_queue_->handle(cmf->GetQueueMessage(200, 0));
                                }
                            read_queue = false;
                        }
                }
        }
}


void ControlThread::keyboard_listener()
{
    bool read_keys = true;
    char c = '0';
    while (read_keys && !stop_)
        {
            std::cin.get(c);
            if (c == 'q')
                {
                    std::cout << "Quit keystroke order received, stopping GNSS-SDR !!" << std::endl;
                    std::unique_ptr<ControlMessageFactory> cmf(new ControlMessageFactory());
                    if (control_queue_ != gr::msg_queue::sptr())
                        {
                            control_queue_->handle(cmf->GetQueueMessage(200, 0));
                        }
                    read_keys = false;
                }
            usleep(500000);
        }
}
