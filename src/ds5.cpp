// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2016 Intel Corporation. All Rights Reserved.

#include <mutex>
#include <chrono>
#include <vector>
#include <iterator>
#include <cstddef>

#include "device.h"
#include "context.h"
#include "image.h"

#include "metadata-parser.h"
#include "ds5.h"
#include "ds5-private.h"
#include "ds5-options.h"
#include "ds5-timestamp.h"

namespace rsimpl2
{
    class ds5_auto_exposure_roi_method : public region_of_interest_method
    {
    public:
        ds5_auto_exposure_roi_method(const hw_monitor& hwm) : _hw_monitor(hwm) {}

        void set(const region_of_interest& roi) override
        {
            command cmd(ds::SETAEROI);
            cmd.param1 = roi.min_y;
            cmd.param2 = roi.max_y;
            cmd.param3 = roi.min_x;
            cmd.param4 = roi.max_x;
            _hw_monitor.send(cmd);
        }

        region_of_interest get() const override
        {
            region_of_interest roi;
            command cmd(ds::GETAEROI);
            auto res = _hw_monitor.send(cmd);

            if (res.size() < 4 * sizeof(uint16_t))
            {
                throw std::runtime_error("Invalid result size!");
            }

            auto words = reinterpret_cast<uint16_t*>(res.data());

            roi.min_y = words[0];
            roi.max_y = words[1];
            roi.min_x = words[2];
            roi.max_x = words[3];

            return roi;
        }

    private:
        const hw_monitor& _hw_monitor;
    };

    class fisheye_auto_exposure_roi_method : public region_of_interest_method
    {
    public:
        fisheye_auto_exposure_roi_method(std::shared_ptr<auto_exposure_mechanism> auto_exposure)
            : _auto_exposure(auto_exposure)
        {}

        void set(const region_of_interest& roi) override
        {
            _auto_exposure->update_auto_exposure_roi(roi);
            _roi = roi;
        }

        region_of_interest get() const override
        {
            return _roi;
        }

    private:
        std::shared_ptr<auto_exposure_mechanism> _auto_exposure;
        region_of_interest _roi{};
    };

    std::shared_ptr<rsimpl2::device> ds5_info::create(const uvc::backend& backend) const
    {
        return std::make_shared<ds5_camera>(backend, _depth, _hwm, _hid);
    }

    std::vector<std::shared_ptr<device_info>> ds5_info::pick_ds5_devices(
        std::shared_ptr<uvc::backend> backend,
        std::vector<uvc::uvc_device_info>& uvc,
        std::vector<uvc::usb_device_info>& usb,
        std::vector<uvc::hid_device_info>& hid)
    {
        std::vector<uvc::uvc_device_info> chosen;
        std::vector<std::shared_ptr<device_info>> results;

        auto valid_pid = filter_by_product(uvc, ds::rs4xx_sku_pid);
        auto group_devices = group_devices_and_hids_by_unique_id(group_devices_by_unique_id(valid_pid), hid);
        for (auto& group : group_devices)
        {
            auto& devices = group.first;
            auto& hids = group.second;

            if((group.first[0].pid == ds::RS430_MM_PID || group.first[0].pid == ds::RS420_MM_PID) &&  hids.size()==0)
                continue;

            if (!devices.empty() &&
                mi_present(devices, 0))
            {
                uvc::usb_device_info hwm;

                std::vector<uvc::usb_device_info> hwm_devices;
                if (ds::try_fetch_usb_device(usb, devices.front(), hwm))
                {
                    hwm_devices.push_back(hwm);
                }
                else
                {
                    LOG_DEBUG("try_fetch_usb_device(...) failed.");
                }

                auto info = std::make_shared<ds5_info>(backend, devices, hwm_devices, hids);
                chosen.insert(chosen.end(), devices.begin(), devices.end());
                results.push_back(info);

            }
            else
            {
                LOG_WARNING("DS5 group_devices is empty.");
            }
        }

        trim_device_list(uvc, chosen);

        return results;
    }

    rs2_motion_device_intrinsic ds5_camera::get_motion_intrinsics(rs2_stream stream) const
    {
        if (stream == RS2_STREAM_ACCEL)
            return create_motion_intrinsics(*_accel_intrinsics);

        if (stream == RS2_STREAM_GYRO)
            return create_motion_intrinsics(*_gyro_intrinsics);

        return device::get_motion_intrinsics(stream);
    }

    std::vector<uint8_t> ds5_camera::send_receive_raw_data(const std::vector<uint8_t>& input)
    {
        return _hw_monitor->send(input);
    }

    void ds5_camera::hardware_reset()
    {
        command cmd(ds::HWRST);
        _hw_monitor->send(cmd);
    }

    rs2_intrinsics ds5_camera::get_intrinsics(unsigned int subdevice, const stream_profile& profile) const
    {
        if (subdevice >= get_endpoints_count())
            throw invalid_value_exception(to_string() << "Requested subdevice " <<
                                          subdevice << " is unsupported.");

        if (subdevice == _depth_device_idx)
        {
            return get_intrinsic_by_resolution(
                *_coefficients_table_raw,
                ds::calibration_table_id::coefficients_table_id,
                profile.width, profile.height);
        }

        if (subdevice == _fisheye_device_idx)
        {
            return get_intrinsic_by_resolution(
                *_fisheye_intrinsics_raw,
                ds::calibration_table_id::fisheye_calibration_id,
                profile.width, profile.height);
        }
        throw not_implemented_exception("Not Implemented");
    }

    pose ds5_camera::get_device_position(unsigned int subdevice) const
    {
        if (subdevice >= get_endpoints_count())
            throw invalid_value_exception(to_string() << "Requested subdevice " <<
                                          subdevice << " is unsupported.");

        if (subdevice == _fisheye_device_idx)
        {
            auto extr = rsimpl2::ds::get_fisheye_extrinsics_data(*_fisheye_extrinsics_raw);
            return inverse(extr);
        }

        if (subdevice == _motion_module_device_idx)
        {
            // Fist, get Fish-eye pose
            auto fe_pose = get_device_position(_fisheye_device_idx);

            auto motion_extr = *_motion_module_extrinsics_raw;

            auto rot = motion_extr.rotation;
            auto trans = motion_extr.translation;

            pose ex = {{rot(0,0), rot(1,0),rot(2,0),rot(1,0), rot(1,1),rot(2,1),rot(0,2), rot(1,2),rot(2,2)},
                       {trans[0], trans[1], trans[2]}};

            return fe_pose * ex;
        }

        throw not_implemented_exception("Not Implemented");
    }

    bool ds5_camera::is_camera_in_advanced_mode() const
    {
        command cmd(ds::UAMG);
        assert(_hw_monitor);
        auto ret = _hw_monitor->send(cmd);
        if (ret.empty())
            throw invalid_value_exception("command result is empty!");

        return (0 != ret.front());
    }

    std::vector<uint8_t> ds5_camera::get_raw_calibration_table(ds::calibration_table_id table_id) const
    {
        command cmd(ds::GETINTCAL, table_id);
        return _hw_monitor->send(cmd);
    }

    std::vector<uint8_t> ds5_camera::get_raw_fisheye_intrinsics_table() const
    {
        const int offset = 0x84;
        const int size = 0x98;
        command cmd(ds::MMER, offset, size);
        return _hw_monitor->send(cmd);
    }

    ds::imu_calibration_table ds5_camera::get_motion_module_calibration_table() const
    {
        const int offset = 0x134;
        const int size = sizeof(ds::imu_calibration_table);
        command cmd(ds::MMER, offset, size);
        auto result = _hw_monitor->send(cmd);
        if (result.size() < sizeof(ds::imu_calibration_table))
            throw std::runtime_error("Not enough data returned from the device!");

        auto table = ds::check_calib<ds::imu_calibration_table>(result);

        return *table;
    }

    std::vector<uint8_t> ds5_camera::get_raw_fisheye_extrinsics_table() const
    {
        command cmd(ds::GET_EXTRINSICS);
        return _hw_monitor->send(cmd);
    }

    std::shared_ptr<hid_endpoint> ds5_camera::create_hid_device(const uvc::backend& backend,
                                                                const std::vector<uvc::hid_device_info>& all_hid_infos,
                                                                const firmware_version& camera_fw_version)
    {
        if (all_hid_infos.empty())
        {
            throw std::runtime_error("HID device is missing!");
        }

        static const char* custom_sensor_fw_ver = "5.6.0.0";
        if (camera_fw_version >= firmware_version(custom_sensor_fw_ver))
        {
            static const std::vector<std::pair<std::string, stream_profile>> custom_sensor_profiles =
                {{std::string("custom"), {RS2_STREAM_GPIO1, 1, 1, 1, RS2_FORMAT_GPIO_RAW}},
                 {std::string("custom"), {RS2_STREAM_GPIO2, 1, 1, 1, RS2_FORMAT_GPIO_RAW}},
                 {std::string("custom"), {RS2_STREAM_GPIO3, 1, 1, 1, RS2_FORMAT_GPIO_RAW}},
                 {std::string("custom"), {RS2_STREAM_GPIO4, 1, 1, 1, RS2_FORMAT_GPIO_RAW}}};
            std::copy(custom_sensor_profiles.begin(), custom_sensor_profiles.end(), std::back_inserter(sensor_name_and_hid_profiles));
        }

        auto hid_ep = std::make_shared<hid_endpoint>(backend.create_hid_device(all_hid_infos.front()),
                                                                               std::unique_ptr<frame_timestamp_reader>(new ds5_iio_hid_timestamp_reader()),
                                                                               std::unique_ptr<frame_timestamp_reader>(new ds5_custom_hid_timestamp_reader()),
                                                                               fps_and_sampling_frequency_per_rs2_stream,
                                                                               sensor_name_and_hid_profiles,
                                                                               backend.create_time_service());
        hid_ep->register_pixel_format(pf_accel_axes);
        hid_ep->register_pixel_format(pf_gyro_axes);


        hid_ep->set_pose(lazy<pose>([](){pose p = {{ { 1,0,0 },{ 0,1,0 },{ 0,0,1 } },{ 0,0,0 }}; return p; }));

        if (camera_fw_version >= firmware_version(custom_sensor_fw_ver))
        {
            hid_ep->register_option(RS2_OPTION_MOTION_MODULE_TEMPERATURE,
                                    std::make_shared<motion_module_temperature_option>(*hid_ep));
            hid_ep->register_pixel_format(pf_gpio_timestamp);
        }

        return hid_ep;
    }

    std::shared_ptr<uvc_endpoint> ds5_camera::create_depth_device(const uvc::backend& backend,
                                                                  const std::vector<uvc::uvc_device_info>& all_device_infos)
    {
        using namespace ds;

        std::vector<std::shared_ptr<uvc::uvc_device>> depth_devices;
        for (auto&& info : filter_by_mi(all_device_infos, 0)) // Filter just mi=0, DEPTH
            depth_devices.push_back(backend.create_uvc_device(info));


        std::unique_ptr<frame_timestamp_reader> ds5_timestamp_reader_backup(new ds5_timestamp_reader(backend.create_time_service()));
        auto depth_ep = std::make_shared<uvc_endpoint>(std::make_shared<uvc::multi_pins_uvc_device>(depth_devices),
                                                       std::unique_ptr<frame_timestamp_reader>(new ds5_timestamp_reader_from_metadata(std::move(ds5_timestamp_reader_backup))),
                                                       backend.create_time_service());
        depth_ep->register_xu(depth_xu); // make sure the XU is initialized everytime we power the camera


        depth_ep->register_pixel_format(pf_z16); // Depth
        depth_ep->register_pixel_format(pf_y8); // Left Only - Luminance
        depth_ep->register_pixel_format(pf_yuyv); // Left Only
        depth_ep->register_pixel_format(pf_uyvyl); // Color from Depth
        depth_ep->register_pixel_format(pf_rgb888);


        // TODO: These if conditions will be implemented as inheritance classes
        auto pid = all_device_infos.front().pid;
        if ((pid == RS410_PID) || (pid == RS430_MM_PID) || (pid == RS430_PID) || (pid == RS430_MM_RGB_PID) || (pid == RS435_RGB_PID))
        {
            depth_ep->register_option(RS2_OPTION_EMITTER_ENABLED, std::make_shared<emitter_option>(*depth_ep));

            depth_ep->register_option(RS2_OPTION_LASER_POWER,
                std::make_shared<uvc_xu_option<uint16_t>>(*depth_ep,
                    depth_xu,
                    DS5_LASER_POWER, "Manual laser power in mw. applicable only when laser power mode is set to Manual"));
        }

        depth_ep->set_pose(lazy<pose>([](){pose p = {{ { 1,0,0 },{ 0,1,0 },{ 0,0,1 } },{ 0,0,0 }}; return p; }));

        return depth_ep;
    }

    std::shared_ptr<uvc_endpoint> ds5_camera::create_color_device(const uvc::backend& backend,
        const std::vector<uvc::uvc_device_info>& color_devices_info)
    {

        std::unique_ptr<frame_timestamp_reader> ds5_timestamp_reader_backup(new ds5_timestamp_reader(backend.create_time_service()));

        auto color_ep = std::make_shared<uvc_endpoint>(backend.create_uvc_device(color_devices_info.front()),
            std::unique_ptr<frame_timestamp_reader>(new ds5_timestamp_reader_from_metadata(std::move(ds5_timestamp_reader_backup))),
            backend.create_time_service());

        _color_device_idx = add_endpoint(color_ep);

        color_ep->register_pixel_format(pf_yuyv);
        color_ep->register_pixel_format(pf_yuy2);
        color_ep->register_pixel_format(pf_bayer16);

        color_ep->register_pu(RS2_OPTION_BACKLIGHT_COMPENSATION);
        color_ep->register_pu(RS2_OPTION_BRIGHTNESS);
        color_ep->register_pu(RS2_OPTION_CONTRAST);
        color_ep->register_pu(RS2_OPTION_EXPOSURE);
        color_ep->register_pu(RS2_OPTION_GAIN);
        color_ep->register_pu(RS2_OPTION_GAMMA);
        color_ep->register_pu(RS2_OPTION_HUE);
        color_ep->register_pu(RS2_OPTION_SATURATION);
        color_ep->register_pu(RS2_OPTION_SHARPNESS);
        color_ep->register_pu(RS2_OPTION_WHITE_BALANCE);
        color_ep->register_pu(RS2_OPTION_ENABLE_AUTO_EXPOSURE);
        color_ep->register_pu(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE);

        return color_ep;
    }

    std::shared_ptr<auto_exposure_mechanism> ds5_camera::register_auto_exposure_options(uvc_endpoint* uvc_ep, const uvc::extension_unit* fisheye_xu)
    {
        auto gain_option =  std::make_shared<uvc_pu_option>(*uvc_ep, RS2_OPTION_GAIN);

        auto exposure_option =  std::make_shared<uvc_xu_option<uint16_t>>(*uvc_ep,
                *fisheye_xu,
                rsimpl2::ds::FISHEYE_EXPOSURE, "Exposure time of Fisheye camera");

        auto ae_state = std::make_shared<auto_exposure_state>();
        auto auto_exposure = std::make_shared<auto_exposure_mechanism>(*gain_option, *exposure_option, *ae_state);

        auto auto_exposure_option = std::make_shared<enable_auto_exposure_option>(uvc_ep,
                                                                                  auto_exposure,
                                                                                  ae_state,
                                                                                  option_range{0, 1, 1, 1});

        uvc_ep->register_option(RS2_OPTION_ENABLE_AUTO_EXPOSURE,auto_exposure_option);

        uvc_ep->register_option(RS2_OPTION_AUTO_EXPOSURE_MODE,
                                std::make_shared<auto_exposure_mode_option>(auto_exposure,
                                                                            ae_state,
                                                                            option_range{0, 2, 1, 0},
                                                                            std::map<float, std::string>{{0.f, "Static"},
                                                                                                         {1.f, "Anti-Flicker"},
                                                                                                         {2.f, "Hybrid"}}));
        uvc_ep->register_option(RS2_OPTION_AUTO_EXPOSURE_ANTIFLICKER_RATE,
                                std::make_shared<auto_exposure_antiflicker_rate_option>(auto_exposure,
                                                                                        ae_state,
                                                                                        option_range{50, 60, 10, 60},
                                                                                        std::map<float, std::string>{{50.f, "50Hz"},
                                                                                                                     {60.f, "60Hz"}}));


        uvc_ep->register_option(RS2_OPTION_GAIN,
                                    std::make_shared<auto_disabling_control>(
                                    gain_option,
                                    auto_exposure_option));

        uvc_ep->register_option(RS2_OPTION_EXPOSURE,
                                    std::make_shared<auto_disabling_control>(
                                    exposure_option,
                                    auto_exposure_option));

        return auto_exposure;
    }

    ds5_camera::ds5_camera(const uvc::backend& backend,
                           const std::vector<uvc::uvc_device_info>& dev_info,
                           const std::vector<uvc::usb_device_info>& hwm_device,
                           const std::vector<uvc::hid_device_info>& hid_info)
        : _depth_device_idx(add_endpoint(create_depth_device(backend, dev_info)))
    {
        using namespace ds;

        if(hwm_device.size()>0)
        {
            _hw_monitor = std::make_shared<hw_monitor>(
                                     std::make_shared<locked_transfer>(
                                        backend.create_usb_device(hwm_device.front()), get_depth_endpoint()));
        }
        else
        {
            _hw_monitor = std::make_shared<hw_monitor>(
                            std::make_shared<locked_transfer>(
                                std::make_shared<command_transfer_over_xu>(
                                    get_depth_endpoint(), rsimpl2::ds::depth_xu, rsimpl2::ds::DS5_HWMONITOR),
                                get_depth_endpoint()));
        }

        _coefficients_table_raw = [this]() { return get_raw_calibration_table(coefficients_table_id); };
        _fisheye_intrinsics_raw = [this]() { return get_raw_fisheye_intrinsics_table(); };
        _fisheye_extrinsics_raw = [this]() { return get_raw_fisheye_extrinsics_table(); };
        _motion_module_extrinsics_raw = [this]() { return get_motion_module_calibration_table().imu_to_fisheye; };
        _accel_intrinsics = [this](){ return get_motion_module_calibration_table().accel_intrinsics; };
        _gyro_intrinsics = [this](){ return get_motion_module_calibration_table().gyro_intrinsics; };

        std::string device_name = (rs4xx_sku_names.end() != rs4xx_sku_names.find(dev_info.front().pid)) ? rs4xx_sku_names.at(dev_info.front().pid) : "RS4xx";
        auto camera_fw_version = firmware_version(_hw_monitor->get_firmware_version_string(GVD, camera_fw_version_offset));
        auto serial = _hw_monitor->get_module_serial_string(GVD, module_serial_offset);


        auto& depth_ep = get_depth_endpoint();
        auto advanced_mode = is_camera_in_advanced_mode();
        if (advanced_mode)
        {
            depth_ep.register_pixel_format(pf_y8i); // L+R
            depth_ep.register_pixel_format(pf_y12i); // L+R - Calibration not rectified
        }

        std::string motion_module_fw_version{""};
        auto pid = dev_info.front().pid;
        auto pid_hex_str = hexify(pid>>8) + hexify(static_cast<uint8_t>(pid));

        std::string is_camera_locked{""};
        if (camera_fw_version >= firmware_version("5.6.3.0"))
        {
            auto is_locked = _hw_monitor->is_camera_locked(GVD, is_camera_locked_offset);
            is_camera_locked = (is_locked)?"YES":"NO";

#ifdef HWM_OVER_XU
            //if hw_monitor was created by usb replace it xu
            if(hwm_device.size() > 0)
            {
                _hw_monitor = std::make_shared<hw_monitor>(
                                std::make_shared<locked_transfer>(
                                    std::make_shared<command_transfer_over_xu>(
                                        get_depth_endpoint(), rsimpl2::ds::depth_xu, rsimpl2::ds::DS5_HWMONITOR),
                                    get_depth_endpoint()));
            }
#endif

            depth_ep.register_pu(RS2_OPTION_GAIN);
            auto exposure_option = std::make_shared<uvc_xu_option<uint32_t>>(depth_ep,
                                                                             depth_xu,
                                                                             DS5_EXPOSURE,
                                                                             "Depth Exposure");
            depth_ep.register_option(RS2_OPTION_EXPOSURE, exposure_option);

            auto enable_auto_exposure = std::make_shared<uvc_xu_option<uint8_t>>(depth_ep,
                                                                                 depth_xu,
                                                                                 DS5_ENABLE_AUTO_EXPOSURE,
                                                                                 "Enable Auto Exposure");
            depth_ep.register_option(RS2_OPTION_ENABLE_AUTO_EXPOSURE, enable_auto_exposure);

            depth_ep.register_option(RS2_OPTION_GAIN,
                                     std::make_shared<auto_disabling_control>(
                                     std::make_shared<uvc_pu_option>(depth_ep, RS2_OPTION_GAIN),
                                     enable_auto_exposure));
            depth_ep.register_option(RS2_OPTION_EXPOSURE,
                                     std::make_shared<auto_disabling_control>(
                                     exposure_option,
                                     enable_auto_exposure));

            // ASR/PRS SKUs support Auto-WB
            if (pid == RS400_PID || pid == RS400_MM_PID || pid == RS410_PID || pid == RS410_MM_PID || pid == RS415_PID)
            {
                depth_ep.register_option(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE,
                    std::make_shared<uvc_xu_option<uint8_t>>(depth_ep,
                                                             depth_xu,
                                                             DS5_ENABLE_AUTO_WHITE_BALANCE,
                                                             "Enable Auto White Balance"));
            }
        }

        if (camera_fw_version >= firmware_version("5.5.8.0"))
        {
             depth_ep.register_option(RS2_OPTION_OUTPUT_TRIGGER_ENABLED,
                                      std::make_shared<uvc_xu_option<uint8_t>>(depth_ep, depth_xu, DS5_EXT_TRIGGER,
                                      "Generate trigger from the camera to external device once per frame"));

             auto error_control = std::unique_ptr<uvc_xu_option<uint8_t>>(new uvc_xu_option<uint8_t>(depth_ep, depth_xu, DS5_ERROR_REPORTING, "Error reporting"));

             _polling_error_handler = std::unique_ptr<polling_error_handler>(
                 new polling_error_handler(1000,
                     std::move(error_control),
                     depth_ep.get_notifications_proccessor(),

                     std::unique_ptr<notification_decoder>(new ds5_notification_decoder())));

             _polling_error_handler->start();

             depth_ep.register_option(RS2_OPTION_ERROR_POLLING_ENABLED, std::make_shared<polling_errors_disable>(_polling_error_handler.get()));

             if (pid == RS410_PID || pid == RS430_MM_PID || pid == RS430_PID)
             {
                 depth_ep.register_option(RS2_OPTION_PROJECTOR_TEMPERATURE,
                                          std::make_shared<asic_and_projector_temperature_options>(depth_ep,
                                                                                                   RS2_OPTION_PROJECTOR_TEMPERATURE));
             }
             depth_ep.register_option(RS2_OPTION_ASIC_TEMPERATURE,
                                      std::make_shared<asic_and_projector_temperature_options>(depth_ep,
                                                                                               RS2_OPTION_ASIC_TEMPERATURE));
             if (pid == RS430_MM_PID || pid == RS420_MM_PID)
                 motion_module_fw_version = _hw_monitor->get_firmware_version_string(GVD, motion_module_fw_version_offset);
         }

        depth_ep.set_roi_method(std::make_shared<ds5_auto_exposure_roi_method>(*_hw_monitor));

        if (advanced_mode)
            depth_ep.register_option(RS2_OPTION_DEPTH_UNITS, std::make_shared<depth_scale_option>(*_hw_monitor));
        else
            depth_ep.register_option(RS2_OPTION_DEPTH_UNITS, std::make_shared<const_value_option>("Number of meters represented by a single depth unit",
                                                                                                  0.001f));
        // Metadata registration
        depth_ep.register_metadata(RS2_FRAME_METADATA_FRAME_TIMESTAMP,    make_uvc_header_parser(&uvc::uvc_header::timestamp));

        // attributes of md_capture_timing
        auto md_prop_offset = offsetof(metadata_raw, mode) +
                offsetof(md_depth_mode, depth_y_mode) +
                offsetof(md_depth_y_normal_mode, intel_capture_timing);

        depth_ep.register_metadata(RS2_FRAME_METADATA_FRAME_COUNTER,    make_attribute_parser(&md_capture_timing::frame_counter, md_capture_timing_attributes::frame_counter_attribute,md_prop_offset));
        depth_ep.register_metadata(RS2_FRAME_METADATA_SENSOR_TIMESTAMP, make_rs4xx_sensor_ts_parser(make_uvc_header_parser(&uvc::uvc_header::timestamp),
                make_attribute_parser(&md_capture_timing::sensor_timestamp, md_capture_timing_attributes::sensor_timestamp_attribute, md_prop_offset)));

        // attributes of md_capture_stats
        md_prop_offset = offsetof(metadata_raw, mode) +
                offsetof(md_depth_mode, depth_y_mode) +
                offsetof(md_depth_y_normal_mode, intel_capture_stats);

        depth_ep.register_metadata(RS2_FRAME_METADATA_WHITE_BALANCE,    make_attribute_parser(&md_capture_stats::white_balance, md_capture_stat_attributes::white_balance_attribute, md_prop_offset));

        // attributes of md_depth_control
        md_prop_offset = offsetof(metadata_raw, mode) +
                offsetof(md_depth_mode, depth_y_mode) +
                offsetof(md_depth_y_normal_mode, intel_depth_control);

        depth_ep.register_metadata(RS2_FRAME_METADATA_GAIN_LEVEL,        make_attribute_parser(&md_depth_control::manual_gain, md_depth_control_attributes::gain_attribute, md_prop_offset));
        depth_ep.register_metadata(RS2_FRAME_METADATA_ACTUAL_EXPOSURE,   make_attribute_parser(&md_depth_control::manual_exposure, md_depth_control_attributes::exposure_attribute, md_prop_offset));
        depth_ep.register_metadata(RS2_FRAME_METADATA_AUTO_EXPOSURE,     make_attribute_parser(&md_depth_control::auto_exposure_mode, md_depth_control_attributes::ae_mode_attribute, md_prop_offset));

        // md_configuration - will be used for internal validation only
        md_prop_offset = offsetof(metadata_raw, mode) + offsetof(md_depth_mode, depth_y_mode) + offsetof(md_depth_y_normal_mode, intel_configuration);

        depth_ep.register_metadata((rs2_frame_metadata)RS2_FRAME_METADATA_HW_TYPE,          make_attribute_parser(&md_configuration::hw_type, md_configuration_attributes::hw_type_attribute, md_prop_offset));
        depth_ep.register_metadata((rs2_frame_metadata)RS2_FRAME_METADATA_SKU_ID,           make_attribute_parser(&md_configuration::sku_id, md_configuration_attributes::sku_id_attribute, md_prop_offset));
        depth_ep.register_metadata((rs2_frame_metadata)RS2_FRAME_METADATA_FORMAT,           make_attribute_parser(&md_configuration::format, md_configuration_attributes::format_attribute, md_prop_offset));
        depth_ep.register_metadata((rs2_frame_metadata)RS2_FRAME_METADATA_WIDTH,            make_attribute_parser(&md_configuration::width, md_configuration_attributes::width_attribute, md_prop_offset));
        depth_ep.register_metadata((rs2_frame_metadata)RS2_FRAME_METADATA_HEIGHT,           make_attribute_parser(&md_configuration::height, md_configuration_attributes::height_attribute, md_prop_offset));

        std::shared_ptr<uvc_endpoint> fisheye_ep;

        if (pid == RS430_MM_PID || pid == RS420_MM_PID)
        {
            auto fisheye_infos = filter_by_mi(dev_info, 3);
            if (fisheye_infos.size() != 1)
                throw invalid_value_exception("RS450 model is expected to include a single fish-eye device!");

            std::unique_ptr<frame_timestamp_reader> ds5_timestamp_reader_backup(new ds5_timestamp_reader(backend.create_time_service()));

            fisheye_ep = std::make_shared<uvc_endpoint>(backend.create_uvc_device(fisheye_infos.front()),
                                                        std::unique_ptr<frame_timestamp_reader>(new ds5_timestamp_reader_from_metadata(std::move(ds5_timestamp_reader_backup))),
                                                        backend.create_time_service());

            fisheye_ep->register_xu(fisheye_xu); // make sure the XU is initialized everytime we power the camera
            fisheye_ep->register_pixel_format(pf_raw8);
            fisheye_ep->register_pixel_format(pf_fe_raw8_unpatched_kernel); // W/O for unpatched kernel

            if (camera_fw_version >= firmware_version("5.6.3.0")) // Create Auto Exposure controls from FW version 5.6.3.0
            {
                auto fisheye_auto_exposure = register_auto_exposure_options(fisheye_ep.get(), &fisheye_xu);
                fisheye_ep->set_roi_method(std::make_shared<fisheye_auto_exposure_roi_method>(fisheye_auto_exposure));
            }
            else
            {
                fisheye_ep->register_option(RS2_OPTION_GAIN,
                                            std::make_shared<uvc_pu_option>(*fisheye_ep.get(),
                                                                            RS2_OPTION_GAIN));
                fisheye_ep->register_option(RS2_OPTION_EXPOSURE,
                                            std::make_shared<uvc_xu_option<uint16_t>>(*fisheye_ep.get(),
                                                                                      fisheye_xu,
                                                                                      rsimpl2::ds::FISHEYE_EXPOSURE,
                                                                                      "Exposure time of Fisheye camera"));
            }

            // Metadata registration
            fisheye_ep->register_metadata(RS2_FRAME_METADATA_FRAME_TIMESTAMP,   make_uvc_header_parser(&uvc::uvc_header::timestamp));
            fisheye_ep->register_metadata(RS2_FRAME_METADATA_AUTO_EXPOSURE,     make_additional_data_parser(&frame_additional_data::fisheye_ae_mode));

            // attributes of md_capture_timing
            md_prop_offset = offsetof(metadata_raw, mode) +
                       offsetof(md_fisheye_mode, fisheye_mode) +
                       offsetof(md_fisheye_normal_mode, intel_capture_timing);

            fisheye_ep->register_metadata(RS2_FRAME_METADATA_FRAME_COUNTER,     make_attribute_parser(&md_capture_timing::frame_counter, md_capture_timing_attributes::frame_counter_attribute,md_prop_offset));
            fisheye_ep->register_metadata(RS2_FRAME_METADATA_SENSOR_TIMESTAMP, make_rs4xx_sensor_ts_parser(make_uvc_header_parser(&uvc::uvc_header::timestamp),
                    make_attribute_parser(&md_capture_timing::sensor_timestamp, md_capture_timing_attributes::sensor_timestamp_attribute, md_prop_offset)));

            // attributes of md_capture_stats
            md_prop_offset = offsetof(metadata_raw, mode) +
                    offsetof(md_fisheye_mode, fisheye_mode) +
                    offsetof(md_fisheye_normal_mode, intel_capture_stats);

            // attributes of md_capture_stats
            md_prop_offset = offsetof(metadata_raw, mode) +
                    offsetof(md_fisheye_mode, fisheye_mode) +
                    offsetof(md_fisheye_normal_mode, intel_configuration);

            fisheye_ep->register_metadata((rs2_frame_metadata)RS2_FRAME_METADATA_HW_TYPE,   make_attribute_parser(&md_configuration::hw_type,    md_configuration_attributes::hw_type_attribute, md_prop_offset));
            fisheye_ep->register_metadata((rs2_frame_metadata)RS2_FRAME_METADATA_SKU_ID,    make_attribute_parser(&md_configuration::sku_id,     md_configuration_attributes::sku_id_attribute, md_prop_offset));
            fisheye_ep->register_metadata((rs2_frame_metadata)RS2_FRAME_METADATA_FORMAT,    make_attribute_parser(&md_configuration::format,     md_configuration_attributes::format_attribute, md_prop_offset));
            fisheye_ep->register_metadata((rs2_frame_metadata)RS2_FRAME_METADATA_WIDTH,     make_attribute_parser(&md_configuration::width,      md_configuration_attributes::width_attribute, md_prop_offset));
            fisheye_ep->register_metadata((rs2_frame_metadata)RS2_FRAME_METADATA_HEIGHT,    make_attribute_parser(&md_configuration::height,     md_configuration_attributes::height_attribute, md_prop_offset));

            // attributes of md_fisheye_control
            md_prop_offset = offsetof(metadata_raw, mode) +
                    offsetof(md_fisheye_mode, fisheye_mode) +
                    offsetof(md_fisheye_normal_mode, intel_fisheye_control);

            fisheye_ep->register_metadata(RS2_FRAME_METADATA_GAIN_LEVEL,        make_attribute_parser(&md_fisheye_control::manual_gain, md_depth_control_attributes::gain_attribute, md_prop_offset));
            fisheye_ep->register_metadata(RS2_FRAME_METADATA_ACTUAL_EXPOSURE,   make_attribute_parser(&md_fisheye_control::manual_exposure, md_depth_control_attributes::exposure_attribute, md_prop_offset));

            // Add fisheye endpoint
            _fisheye_device_idx = add_endpoint(fisheye_ep);

            fisheye_ep->set_pose(lazy<pose>([this](){return get_device_position(_fisheye_device_idx);}));

            // Add hid endpoint
            auto hid_ep = create_hid_device(backend, hid_info, camera_fw_version);
            _motion_module_device_idx = add_endpoint(hid_ep);

            try
            {
                hid_ep->register_option(RS2_OPTION_ENABLE_MOTION_CORRECTION,
                                        std::make_shared<enable_motion_correction>(hid_ep.get(),
                                                                                   *_accel_intrinsics,
                                                                                   *_gyro_intrinsics,
                                                                                   option_range{0, 1, 1, 1}));
            }
            catch (const std::exception& ex)
            {
                LOG_ERROR("Motion Device is not calibrated! Motion Data Correction will not be available! Error: " << ex.what());
            }

            for (auto& elem : hid_info)
            {
                std::map<rs2_camera_info, std::string> camera_info = {{RS2_CAMERA_INFO_DEVICE_NAME, device_name},
                                                                      {RS2_CAMERA_INFO_MODULE_NAME, "Motion Module"},
                                                                      {RS2_CAMERA_INFO_DEVICE_SERIAL_NUMBER, serial},
                                                                      {RS2_CAMERA_INFO_CAMERA_FIRMWARE_VERSION, static_cast<const char*>(camera_fw_version)},
                                                                      {RS2_CAMERA_INFO_DEVICE_LOCATION, elem.device_path},
                                                                      {RS2_CAMERA_INFO_DEVICE_DEBUG_OP_CODE, std::to_string(static_cast<int>(fw_cmd::GLD))},
                                                                      {RS2_CAMERA_INFO_PRODUCT_ID, pid_hex_str}};
                if (!motion_module_fw_version.empty())
                    camera_info[RS2_CAMERA_INFO_MOTION_MODULE_FIRMWARE_VERSION] = motion_module_fw_version;

                if (!is_camera_locked.empty())
                    camera_info[RS2_CAMERA_INFO_IS_CAMERA_LOCKED] = is_camera_locked;

                register_endpoint_info(_motion_module_device_idx, camera_info);
                hid_ep->set_pose(lazy<pose>([this](){return get_device_position(_motion_module_device_idx); }));
            }
        }

        std::shared_ptr<uvc_endpoint> color_ep;
        // Add RGB Sensor
        if ((pid == RS415_PID) || (pid == RS430_MM_RGB_PID) || (pid == RS435_RGB_PID))
        {
            auto color_devs_info = filter_by_mi(dev_info, 3); // TODO check
            if (color_devs_info.size() != 1)
                throw invalid_value_exception(to_string() << "RS4XX with RGB models are expected to include a single color device! - "
                    << color_devs_info.size() << " found");

            color_ep = create_color_device(backend, color_devs_info);
            color_ep->set_pose(lazy<pose>([]() {return pose{ { { 1,0,0 },{ 0,1,0 },{ 0,0,1 } },{ 0,0,0 } }; })); // TODO: Fetch calibration extrinsic
        }

        // Register endpoint info
        for(auto& element : dev_info)
        {
            if (element.mi == 0) // mi 0 is defines RS4xx Stereo (Depth) interface
            {
                std::map<rs2_camera_info, std::string> camera_info = {{RS2_CAMERA_INFO_DEVICE_NAME, device_name},
                                                                      {RS2_CAMERA_INFO_MODULE_NAME, "Stereo Module"},
                                                                      {RS2_CAMERA_INFO_DEVICE_SERIAL_NUMBER, serial},
                                                                      {RS2_CAMERA_INFO_CAMERA_FIRMWARE_VERSION, static_cast<const char*>(camera_fw_version)},
                                                                      {RS2_CAMERA_INFO_DEVICE_LOCATION, element.device_path},
                                                                      {RS2_CAMERA_INFO_DEVICE_DEBUG_OP_CODE, std::to_string(static_cast<int>(fw_cmd::GLD))},
                                                                      {RS2_CAMERA_INFO_ADVANCED_MODE, ((advanced_mode)?"YES":"NO")},
                                                                      {RS2_CAMERA_INFO_PRODUCT_ID, pid_hex_str}};
                if (!motion_module_fw_version.empty())
                    camera_info[RS2_CAMERA_INFO_MOTION_MODULE_FIRMWARE_VERSION] = motion_module_fw_version;

                if (!is_camera_locked.empty())
                    camera_info[RS2_CAMERA_INFO_IS_CAMERA_LOCKED] = is_camera_locked;

                register_endpoint_info(_depth_device_idx, camera_info);
            }
            else if (fisheye_ep && (element.pid == RS430_MM_PID || element.pid == RS420_MM_PID) && element.mi == 3) // mi 3 is related to Fisheye device
            {
                std::map<rs2_camera_info, std::string> camera_info = {{RS2_CAMERA_INFO_DEVICE_NAME, device_name},
                                                                      {RS2_CAMERA_INFO_MODULE_NAME, "Fisheye Camera"},
                                                                      {RS2_CAMERA_INFO_DEVICE_SERIAL_NUMBER, serial},
                                                                      {RS2_CAMERA_INFO_CAMERA_FIRMWARE_VERSION, static_cast<const char*>(camera_fw_version)},
                                                                      {RS2_CAMERA_INFO_DEVICE_LOCATION, element.device_path},
                                                                      {RS2_CAMERA_INFO_PRODUCT_ID, pid_hex_str}};
                if (!motion_module_fw_version.empty())
                    camera_info[RS2_CAMERA_INFO_MOTION_MODULE_FIRMWARE_VERSION] = motion_module_fw_version;

                if (!is_camera_locked.empty())
                    camera_info[RS2_CAMERA_INFO_IS_CAMERA_LOCKED] = is_camera_locked;

                register_endpoint_info(_fisheye_device_idx, camera_info);
            }
            else if (color_ep && ((element.pid == RS415_PID) || (element.pid == RS435_RGB_PID)) && element.mi == 3) // mi 3 is related to Color device
            {
                std::map<rs2_camera_info, std::string> camera_info = { { RS2_CAMERA_INFO_DEVICE_NAME, device_name },
                { RS2_CAMERA_INFO_MODULE_NAME, "RGB Camera" },
                { RS2_CAMERA_INFO_DEVICE_SERIAL_NUMBER, serial },
                { RS2_CAMERA_INFO_CAMERA_FIRMWARE_VERSION, static_cast<const char*>(camera_fw_version) },
                { RS2_CAMERA_INFO_DEVICE_LOCATION, element.device_path },
                { RS2_CAMERA_INFO_PRODUCT_ID, pid_hex_str } };
                register_endpoint_info(_color_device_idx, camera_info);
            }
        }
    }

    notification ds5_notification_decoder::decode(int value)
    {
        if (value == 0)
            return{ RS2_NOTIFICATION_CATEGORY_HARDWARE_ERROR, value, RS2_LOG_SEVERITY_ERROR, "Success" };
        if (value == ds::ds5_notifications_types::hot_laser_power_reduce)
            return{ RS2_NOTIFICATION_CATEGORY_HARDWARE_ERROR, value, RS2_LOG_SEVERITY_ERROR, "Hot laser power reduce" };
        if (value == ds::ds5_notifications_types::hot_laser_disable)
            return{ RS2_NOTIFICATION_CATEGORY_HARDWARE_ERROR, value, RS2_LOG_SEVERITY_ERROR, "Hot laser disable" };
        if (value == ds::ds5_notifications_types::flag_B_laser_disable)
            return{ RS2_NOTIFICATION_CATEGORY_HARDWARE_ERROR, value, RS2_LOG_SEVERITY_ERROR, "Flag B laser disable" };

        return{ RS2_NOTIFICATION_CATEGORY_HARDWARE_ERROR, value, RS2_LOG_SEVERITY_NONE, "Unknown error!" };
    }

    rs2_extrinsics ds5_camera::get_extrinsics(int from_subdevice, rs2_stream from_stream, int to_subdevice, rs2_stream to_stream)
    {
        auto is_left = [](rs2_stream s) { return s == RS2_STREAM_INFRARED || s == RS2_STREAM_DEPTH; };

        if (from_subdevice == to_subdevice && from_subdevice == 0)
        {
            rs2_extrinsics ext { {1,0,0,0,1,0,0,0,1}, {0,0,0} };

            if (is_left(to_stream) && from_stream == RS2_STREAM_INFRARED2)
            {
                auto table = ds::check_calib<ds::coefficients_table>(*_coefficients_table_raw);
                ext.translation[0] = -0.001f * table->baseline;
                return ext;
            }
            else if (to_stream == RS2_STREAM_INFRARED2 && is_left(from_stream))
            {
                auto table = ds::check_calib<ds::coefficients_table>(*_coefficients_table_raw);
                ext.translation[0] = 0.001f * table->baseline;
                return ext;
            }
        }
        return device::get_extrinsics(from_subdevice, from_stream, to_subdevice, to_stream);
    }
}
