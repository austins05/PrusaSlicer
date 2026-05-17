///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_BambuCloud_hpp_
#define slic3r_BambuCloud_hpp_

#include <string>
#include <vector>

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
};

} // namespace Slic3r

#endif
