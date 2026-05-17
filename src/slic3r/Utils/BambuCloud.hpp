///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_BambuCloud_hpp_
#define slic3r_BambuCloud_hpp_

#include <string>
#include <vector>
#include <functional>

namespace Slic3r {

struct BambuCloudStatus
{
    bool loaded { false };
    bool agent_created { false };
    bool logged_in { false };
    std::string plugin_dir;
    std::string library_path;
    std::string version;
    std::string host;
    std::string user_id;
    std::string user_name;
    std::string user_nickname;
    std::string error;
};

struct BambuCloudPrintParams
{
    std::string dev_id;
    std::string task_name;
    std::string project_name;
    std::string preset_name;
    std::string filename;
    std::string config_filename;
    int plate_index { 0 };
    std::string ftp_folder;
    std::string ftp_file;
    std::string ftp_file_md5;
    std::string nozzle_mapping;
    std::string ams_mapping;
    std::string ams_mapping2;
    std::string ams_mapping_info;
    std::string nozzles_info;
    std::string connection_type;
    std::string comments;
    int origin_profile_id { 0 };
    int stl_design_id { 0 };
    std::string origin_model_id;
    std::string print_type;
    std::string dst_file;
    std::string dev_name;
    std::string dev_ip;
    bool use_ssl_for_ftp { true };
    bool use_ssl_for_mqtt { true };
    std::string username;
    std::string password;
    bool task_bed_leveling { true };
    bool task_flow_cali { false };
    bool task_vibration_cali { false };
    bool task_layer_inspect { false };
    bool task_record_timelapse { false };
    bool task_use_ams { false };
    std::string task_bed_type;
    std::string extra_options;
    int auto_bed_leveling { 0 };
    int auto_flow_cali { 0 };
    int auto_offset_cali { 0 };
    int extruder_cali_manual_mode { -1 };
    bool task_ext_change_assist { false };
    bool try_emmc_print { false };
};

class BambuCloud
{
public:
    static BambuCloud& instance();

    bool import_orca_plugin(std::string &error);
    bool initialize(const std::string &country_code, std::string &error);
    void shutdown();

    bool is_loaded() const { return m_module != nullptr; }
    bool has_agent() const { return m_agent != nullptr; }
    bool is_user_login() const;
    bool change_user(const std::string &user_info, std::string &error);
    bool logout(bool request, std::string &error);
    bool get_user_print_info(std::string &body, unsigned int &http_code, std::string &error);
    bool query_bind_status(const std::vector<std::string> &device_ids, std::string &body, unsigned int &http_code, std::string &error);
    bool get_printer_firmware(const std::string &device_id, std::string &body, unsigned int &http_code, std::string &error);
    bool get_camera_url(const std::string &device_id, std::string &url, std::string &error);
    bool get_camera_url_async(const std::string &device_id, std::function<void(std::string)> callback, std::string &error);
    bool send_cloud_message(const std::string &device_id, const std::string &json, int qos, int flag, std::string &error);
    bool start_subscribe(const std::string &module, std::string &error);
    bool add_subscribe(const std::vector<std::string> &device_ids, std::string &error);
    bool set_message_callback(std::function<void(std::string, std::string)> callback, std::string &error);
    bool start_print(const BambuCloudPrintParams &params, std::function<void(int, int, std::string)> progress, std::string &error);

    std::string build_login_cmd() const;
    std::string build_login_info() const;
    std::string cloud_host() const;
    std::string cloud_login_url(const std::string &language = std::string()) const;
    BambuCloudStatus status() const;

private:
    BambuCloud() = default;
    ~BambuCloud();

    BambuCloud(const BambuCloud&) = delete;
    BambuCloud& operator=(const BambuCloud&) = delete;

    using func_get_version = std::string (*)();
    using func_create_agent = void* (*)(std::string);
    using func_destroy_agent = int (*)(void*);
    using func_init_log = int (*)(void*);
    using func_set_config_dir = int (*)(void*, std::string);
    using func_set_cert_file = int (*)(void*, std::string, std::string);
    using func_set_country_code = int (*)(void*, std::string);
    using func_start = int (*)(void*);
    using func_change_user = int (*)(void*, std::string);
    using func_is_user_login = bool (*)(void*);
    using func_user_logout = int (*)(void*, bool);
    using func_get_user_string = std::string (*)(void*);
    using func_build_string = std::string (*)(void*);
    using func_connect_server = int (*)(void*);
    using func_get_user_print_info = int (*)(void*, unsigned int*, std::string*);
    using func_query_bind_status = int (*)(void*, std::vector<std::string>, unsigned int*, std::string*);
    using func_get_printer_firmware = int (*)(void*, std::string, unsigned int*, std::string*);
    using func_get_camera_url = int (*)(void*, std::string, std::function<void(std::string)>);
    using func_send_message = int (*)(void*, std::string, std::string, int, int);
    using func_start_subscribe = int (*)(void*, std::string);
    using func_add_subscribe = int (*)(void*, std::vector<std::string>);
    using func_set_on_message_fn = int (*)(void*, std::function<void(std::string, std::string)>);
    using func_start_print = int (*)(void*, BambuCloudPrintParams, std::function<void(int, int, std::string)>, std::function<bool()>, std::function<bool(int, std::string)>);

    void* get_symbol(const char *name) const;
    bool load_functions(std::string &error);
    bool create_agent(std::string &error);
    std::string choose_library_path(std::string &error) const;

    void *m_module { nullptr };
    void *m_agent { nullptr };
    std::string m_library_path;
    std::string m_error;

    func_get_version m_get_version { nullptr };
    func_create_agent m_create_agent { nullptr };
    func_destroy_agent m_destroy_agent { nullptr };
    func_init_log m_init_log { nullptr };
    func_set_config_dir m_set_config_dir { nullptr };
    func_set_cert_file m_set_cert_file { nullptr };
    func_set_country_code m_set_country_code { nullptr };
    func_start m_start { nullptr };
    func_change_user m_change_user { nullptr };
    func_is_user_login m_is_user_login { nullptr };
    func_user_logout m_user_logout { nullptr };
    func_get_user_string m_get_user_id { nullptr };
    func_get_user_string m_get_user_name { nullptr };
    func_get_user_string m_get_user_nickname { nullptr };
    func_build_string m_build_login_cmd { nullptr };
    func_build_string m_build_login_info { nullptr };
    func_build_string m_get_bambulab_host { nullptr };
    func_connect_server m_connect_server { nullptr };
    func_get_user_print_info m_get_user_print_info { nullptr };
    func_query_bind_status m_query_bind_status { nullptr };
    func_get_printer_firmware m_get_printer_firmware { nullptr };
    func_get_camera_url m_get_camera_url { nullptr };
    func_send_message m_send_message { nullptr };
    func_start_subscribe m_start_subscribe { nullptr };
    func_add_subscribe m_add_subscribe { nullptr };
    func_set_on_message_fn m_set_on_message_fn { nullptr };
    func_start_print m_start_print { nullptr };
};

} // namespace Slic3r

#endif
