///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BambuCloud.hpp"

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cstdlib>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace fs = boost::filesystem;

namespace Slic3r {

namespace {

fs::path plugin_dir()
{
    return fs::path(data_dir()) / "plugins";
}

bool is_bambu_runtime_file(const fs::path &path)
{
    const std::string name = path.filename().string();
    return name.find("bambu_networking") != std::string::npos ||
           name.find("BambuSource") != std::string::npos ||
           name.find("live555") != std::string::npos;
}

std::vector<fs::path> candidate_orca_plugin_dirs()
{
    std::vector<fs::path> dirs;
    if (const char *home = std::getenv("HOME"); home != nullptr) {
        dirs.emplace_back(fs::path(home) / ".config" / "OrcaSlicer" / "plugins");
        dirs.emplace_back(fs::path(home) / ".config" / "BambuStudio" / "plugins");
    }
    return dirs;
}

} // namespace

BambuCloud& BambuCloud::instance()
{
    static BambuCloud cloud;
    return cloud;
}

BambuCloud::~BambuCloud()
{
    shutdown();
}

void BambuCloud::shutdown()
{
    if (m_agent != nullptr && m_destroy_agent != nullptr) {
        try {
            m_destroy_agent(m_agent);
        } catch (...) {
        }
    }
    m_agent = nullptr;

#if !defined(_WIN32)
    if (m_module != nullptr)
        dlclose(m_module);
#endif
    m_module = nullptr;
    m_library_path.clear();
    m_error.clear();
    m_get_version = nullptr;
    m_create_agent = nullptr;
    m_destroy_agent = nullptr;
    m_init_log = nullptr;
    m_set_config_dir = nullptr;
    m_set_cert_file = nullptr;
    m_set_country_code = nullptr;
    m_start = nullptr;
    m_change_user = nullptr;
    m_is_user_login = nullptr;
    m_user_logout = nullptr;
    m_get_user_id = nullptr;
    m_get_user_name = nullptr;
    m_get_user_nickname = nullptr;
    m_build_login_cmd = nullptr;
    m_build_login_info = nullptr;
    m_get_bambulab_host = nullptr;
    m_connect_server = nullptr;
}

bool BambuCloud::import_orca_plugin(std::string &error)
{
    error.clear();
    const fs::path dst_dir = plugin_dir();
    try {
        fs::create_directories(dst_dir);
        for (const fs::path &src_dir : candidate_orca_plugin_dirs()) {
            if (!fs::exists(src_dir) || !fs::is_directory(src_dir))
                continue;
            size_t copied = 0;
            for (const fs::directory_entry &entry : fs::directory_iterator(src_dir)) {
                if (!fs::is_regular_file(entry.path()) || !is_bambu_runtime_file(entry.path()))
                    continue;
                fs::copy_file(entry.path(), dst_dir / entry.path().filename(), fs::copy_option::overwrite_if_exists);
                ++copied;
            }
            if (copied > 0)
                return true;
        }
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
    error = "No OrcaSlicer or BambuStudio Bambu networking plugin files were found.";
    return false;
}

std::string BambuCloud::choose_library_path(std::string &error) const
{
    error.clear();
    const fs::path dir = plugin_dir();
    if (!fs::exists(dir)) {
        error = "Plugin directory does not exist: " + dir.string();
        return {};
    }

    std::vector<fs::path> versioned;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir)) {
        if (!fs::is_regular_file(entry.path()))
            continue;
        const std::string name = entry.path().filename().string();
#if defined(__APPLE__)
        const bool is_network = name.find("libbambu_networking") == 0 && name.rfind(".dylib") != std::string::npos;
#else
        const bool is_network = name.find("libbambu_networking") == 0 && name.rfind(".so") != std::string::npos;
#endif
        if (!is_network)
            continue;
        if (name.find("_") != std::string::npos)
            versioned.push_back(entry.path());
    }

    if (!versioned.empty()) {
        std::sort(versioned.begin(), versioned.end());
        return versioned.back().string();
    }

#if defined(__APPLE__)
    const fs::path fallback = dir / "libbambu_networking.dylib";
#else
    const fs::path fallback = dir / "libbambu_networking.so";
#endif
    if (fs::exists(fallback))
        return fallback.string();

    error = "No libbambu_networking runtime was found in " + dir.string();
    return {};
}

void* BambuCloud::get_symbol(const char *name) const
{
#if defined(_WIN32)
    return nullptr;
#else
    return m_module == nullptr ? nullptr : dlsym(m_module, name);
#endif
}

bool BambuCloud::load_functions(std::string &error)
{
    m_get_version = reinterpret_cast<func_get_version>(get_symbol("bambu_network_get_version"));
    m_create_agent = reinterpret_cast<func_create_agent>(get_symbol("bambu_network_create_agent"));
    m_destroy_agent = reinterpret_cast<func_destroy_agent>(get_symbol("bambu_network_destroy_agent"));
    m_init_log = reinterpret_cast<func_init_log>(get_symbol("bambu_network_init_log"));
    m_set_config_dir = reinterpret_cast<func_set_config_dir>(get_symbol("bambu_network_set_config_dir"));
    m_set_cert_file = reinterpret_cast<func_set_cert_file>(get_symbol("bambu_network_set_cert_file"));
    m_set_country_code = reinterpret_cast<func_set_country_code>(get_symbol("bambu_network_set_country_code"));
    m_start = reinterpret_cast<func_start>(get_symbol("bambu_network_start"));
    m_change_user = reinterpret_cast<func_change_user>(get_symbol("bambu_network_change_user"));
    m_is_user_login = reinterpret_cast<func_is_user_login>(get_symbol("bambu_network_is_user_login"));
    m_user_logout = reinterpret_cast<func_user_logout>(get_symbol("bambu_network_user_logout"));
    m_get_user_id = reinterpret_cast<func_get_user_string>(get_symbol("bambu_network_get_user_id"));
    m_get_user_name = reinterpret_cast<func_get_user_string>(get_symbol("bambu_network_get_user_name"));
    m_get_user_nickname = reinterpret_cast<func_get_user_string>(get_symbol("bambu_network_get_user_nickanme"));
    m_build_login_cmd = reinterpret_cast<func_build_string>(get_symbol("bambu_network_build_login_cmd"));
    m_build_login_info = reinterpret_cast<func_build_string>(get_symbol("bambu_network_build_login_info"));
    m_get_bambulab_host = reinterpret_cast<func_build_string>(get_symbol("bambu_network_get_bambulab_host"));
    m_connect_server = reinterpret_cast<func_connect_server>(get_symbol("bambu_network_connect_server"));

    if (m_create_agent == nullptr || m_destroy_agent == nullptr || m_change_user == nullptr ||
        m_is_user_login == nullptr || m_build_login_cmd == nullptr || m_build_login_info == nullptr) {
        error = "Bambu network runtime is missing required login symbols.";
        return false;
    }
    return true;
}

bool BambuCloud::create_agent(std::string &error)
{
    if (m_agent != nullptr)
        return true;
    if (m_create_agent == nullptr) {
        error = "Bambu network runtime was not initialized.";
        return false;
    }
    m_agent = m_create_agent(data_dir());
    if (m_agent == nullptr) {
        error = "Bambu network runtime failed to create an agent.";
        return false;
    }
    return true;
}

bool BambuCloud::initialize(const std::string &country_code, std::string &error)
{
    error.clear();
    if (m_module == nullptr) {
        m_library_path = choose_library_path(error);
        if (m_library_path.empty()) {
            m_error = error;
            return false;
        }
#if defined(_WIN32)
        error = "Bambu cloud plugin loading is currently implemented for Linux/macOS builds only.";
        m_error = error;
        return false;
#else
        m_module = dlopen(m_library_path.c_str(), RTLD_LAZY);
        if (m_module == nullptr) {
            const char *dl_error = dlerror();
            error = dl_error != nullptr ? dl_error : "dlopen failed for Bambu network runtime.";
            m_error = error;
            return false;
        }
#endif
        if (!load_functions(error)) {
            m_error = error;
            return false;
        }
    }

    if (!create_agent(error)) {
        m_error = error;
        return false;
    }

    if (m_set_config_dir != nullptr)
        m_set_config_dir(m_agent, data_dir());
    if (m_init_log != nullptr)
        m_init_log(m_agent);
    if (m_set_cert_file != nullptr)
        m_set_cert_file(m_agent, (fs::path(resources_dir()) / "cert").string(), "slicer_base64.cer");
    if (m_set_country_code != nullptr)
        m_set_country_code(m_agent, country_code.empty() ? "US" : country_code);
    if (m_start != nullptr)
        m_start(m_agent);
    if (m_connect_server != nullptr)
        m_connect_server(m_agent);

    m_error.clear();
    return true;
}

bool BambuCloud::is_user_login() const
{
    return m_agent != nullptr && m_is_user_login != nullptr && m_is_user_login(m_agent);
}

bool BambuCloud::change_user(const std::string &user_info, std::string &error)
{
    error.clear();
    if (m_agent == nullptr || m_change_user == nullptr) {
        error = "Bambu cloud agent is not initialized.";
        return false;
    }
    const int result = m_change_user(m_agent, user_info);
    if (result != 0) {
        error = "Bambu cloud login failed with result " + std::to_string(result) + ".";
        return false;
    }
    return true;
}

bool BambuCloud::logout(bool request, std::string &error)
{
    error.clear();
    if (m_agent == nullptr || m_user_logout == nullptr) {
        error = "Bambu cloud agent is not initialized.";
        return false;
    }
    const int result = m_user_logout(m_agent, request);
    if (result != 0) {
        error = "Bambu cloud logout failed with result " + std::to_string(result) + ".";
        return false;
    }
    return true;
}

std::string BambuCloud::build_login_cmd() const
{
    return m_agent != nullptr && m_build_login_cmd != nullptr ? m_build_login_cmd(m_agent) : std::string();
}

std::string BambuCloud::build_login_info() const
{
    return m_agent != nullptr && m_build_login_info != nullptr ? m_build_login_info(m_agent) : std::string();
}

std::string BambuCloud::cloud_host() const
{
    return m_agent != nullptr && m_get_bambulab_host != nullptr ? m_get_bambulab_host(m_agent) : std::string("https://api.bambulab.com/");
}

std::string BambuCloud::cloud_login_url(const std::string &language) const
{
    std::string host = cloud_host();
    while (!host.empty() && host.back() == '/')
        host.pop_back();
    if (language.empty())
        return host + "/sign-in";
    return host + "/" + language + "/sign-in";
}

BambuCloudStatus BambuCloud::status() const
{
    BambuCloudStatus out;
    out.loaded = m_module != nullptr;
    out.agent_created = m_agent != nullptr;
    out.logged_in = is_user_login();
    out.plugin_dir = plugin_dir().string();
    out.library_path = m_library_path;
    out.error = m_error;
    out.version = m_module != nullptr && m_get_version != nullptr ? m_get_version() : std::string();
    out.host = cloud_host();
    out.user_id = m_agent != nullptr && m_get_user_id != nullptr ? m_get_user_id(m_agent) : std::string();
    out.user_name = m_agent != nullptr && m_get_user_name != nullptr ? m_get_user_name(m_agent) : std::string();
    out.user_nickname = m_agent != nullptr && m_get_user_nickname != nullptr ? m_get_user_nickname(m_agent) : std::string();
    return out;
}

} // namespace Slic3r
