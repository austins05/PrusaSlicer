///|/ Copyright (c) Prusa Research 2018 - 2023
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_BambuLan_hpp_
#define slic3r_BambuLan_hpp_

#include <string>
#include <vector>

#include "PrintHost.hpp"

namespace Slic3r {

class DynamicPrintConfig;

class BambuLan : public PrintHost
{
public:
    explicit BambuLan(DynamicPrintConfig *config);
    ~BambuLan() override = default;

    const char* get_name() const override { return "Bambu Lab LAN"; }

    bool test(wxString &msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return true; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }
    std::string get_notification_host() const override { return "Bambu Lab LAN " + m_host; }
    std::string get_unusable_symbols() const override { return "\\/:*?\"<>|"; }

    bool request_status(std::string &status_json, std::string &error) const;
    bool send_print_command(const std::string &command, std::string &error) const;
    bool send_pushing_command(const std::string &command, std::string &error) const;
    bool send_gcode_line(const std::string &gcode, std::string &error) const;
    bool set_bed_temp(int temp, std::string &error) const;
    bool set_nozzle_temp(int temp, std::string &error) const;
    bool set_chamber_temp(int temp, std::string &error) const;
    bool set_print_speed(int speed_level, std::string &error) const;
    bool set_camera_recording(bool enabled, std::string &error) const;
    bool set_camera_timelapse(bool enabled, std::string &error) const;
    bool set_camera_resolution(const std::string &resolution, std::string &error) const;
    bool set_print_option(const std::string &option, bool enabled, std::string &error) const;
    bool set_xcam_module(const std::string &module, bool enabled, const std::string &sensitivity, std::string &error) const;
    bool send_ams_control(const std::string &action, std::string &error) const;
    bool ams_change_filament(bool load, int ams_id, int slot_id, int current_temp, int target_temp, std::string &error) const;
    bool ams_refresh_rfid(int ams_id, int slot_id, std::string &error) const;
    bool ams_calibrate(int ams_id, std::string &error) const;
    bool list_sdcard(std::vector<std::string> &files, std::string &error) const;
    bool delete_sdcard_file(const std::string &filename, std::string &error) const;
    bool start_sdcard_file(const std::string &filename, const std::string &print_options, std::string &error) const;

private:
    bool ftps_test(std::string &error) const;
    bool ftps_upload(const boost::filesystem::path &source_path, const std::string &remote_filename, ProgressFn progress_fn, std::string &error) const;
    bool make_bambu_project_archive(const boost::filesystem::path &source_path, boost::filesystem::path &archive_path, std::string &error) const;
    bool mqtt_start_print(const std::string &remote_filename, const std::string &print_options, std::string &error) const;
    bool mqtt_publish_json(const std::string &payload, std::string &error) const;

    std::string m_host;
    std::string m_access_code;
    std::string m_serial;
};

}

#endif
