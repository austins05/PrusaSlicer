///|/ Copyright (c) Prusa Research 2018 - 2023
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BambuLan.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>

#include <curl/curl.h>
#include <wx/string.h>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include "libslic3r/PrintConfig.hpp"
#include "miniz.h"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace {

constexpr const char *BAMBU_LAN_USER = "bblp";
constexpr unsigned BAMBU_FTPS_PORT = 990;
constexpr unsigned BAMBU_MQTT_PORT = 8883;

struct BambuStartOptions
{
    bool use_ams = false;
    std::string ams_mapping;
    std::string bed_type;
    bool bed_leveling = true;
    bool flow_cali = false;
    bool vibration_cali = false;
    bool layer_inspect = false;
    bool timelapse = false;
};

std::string trim_host(std::string host)
{
    while (!host.empty() && (host.back() == '/' || host.back() == ' '))
        host.pop_back();
    const std::string ftps = "ftps://";
    const std::string https = "https://";
    const std::string http = "http://";
    if (host.rfind(ftps, 0) == 0)
        host.erase(0, ftps.size());
    else if (host.rfind(https, 0) == 0)
        host.erase(0, https.size());
    else if (host.rfind(http, 0) == 0)
        host.erase(0, http.size());
    const size_t slash = host.find('/');
    if (slash != std::string::npos)
        host.erase(slash);
    return host;
}

std::string trim_copy(std::string text)
{
    const auto is_not_space = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), is_not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), is_not_space).base(), text.end());
    return text;
}

std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool parse_bool(std::string value, bool default_value)
{
    value = lowercase(trim_copy(std::move(value)));
    if (value == "1" || value == "true" || value == "yes" || value == "on")
        return true;
    if (value == "0" || value == "false" || value == "no" || value == "off")
        return false;
    return default_value;
}

std::map<std::string, std::string> parse_option_lines(const std::string &text)
{
    std::map<std::string, std::string> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t equal = line.find('=');
        if (equal == std::string::npos)
            continue;
        out[trim_copy(line.substr(0, equal))] = trim_copy(line.substr(equal + 1));
    }
    return out;
}

bool raw_json_array_is_safe(const std::string &text)
{
    if (text.empty() || text.front() != '[' || text.back() != ']')
        return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isdigit(c) || c == '[' || c == ']' || c == ',' || c == ' ' || c == '-';
    });
}

std::string normalize_ams_mapping(std::string mapping)
{
    mapping = trim_copy(std::move(mapping));
    if (mapping.empty())
        return {};
    if (mapping.front() != '[')
        mapping = "[" + mapping + "]";
    return raw_json_array_is_safe(mapping) ? mapping : std::string();
}

BambuStartOptions parse_bambu_start_options(const std::string &text)
{
    BambuStartOptions options;
    const auto values = parse_option_lines(text);
    auto get = [&values](const char *key) -> std::string {
        auto it = values.find(key);
        return it == values.end() ? std::string() : it->second;
    };

    options.use_ams = parse_bool(get("bambu_use_ams"), options.use_ams);
    options.ams_mapping = normalize_ams_mapping(get("bambu_ams_mapping"));
    options.bed_leveling = parse_bool(get("bambu_bed_leveling"), options.bed_leveling);
    options.flow_cali = parse_bool(get("bambu_flow_cali"), options.flow_cali);
    options.vibration_cali = parse_bool(get("bambu_vibration_cali"), options.vibration_cali);
    options.layer_inspect = parse_bool(get("bambu_layer_inspect"), options.layer_inspect);
    options.timelapse = parse_bool(get("bambu_timelapse"), options.timelapse);

    const std::string bed_type = get("bambu_bed_type");
    if (bed_type == "1")
        options.bed_type = "textured_plate";
    else if (bed_type == "2")
        options.bed_type = "cool_plate";
    else if (bed_type == "3")
        options.bed_type = "eng_plate";
    else if (bed_type == "4")
        options.bed_type = "hot_plate";

    return options;
}

std::string sanitize_remote_filename(std::string filename)
{
    if (filename.empty())
        filename = "prusaslicer.gcode";
    for (char &c : filename)
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32)
            c = '_';
    return filename;
}

bool ends_with(const std::string &text, const std::string &suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string bambu_project_filename(std::string filename)
{
    filename = sanitize_remote_filename(std::move(filename));
    const std::string lower = lowercase(filename);
    if (ends_with(lower, ".gcode.3mf"))
        return filename;
    if (ends_with(lower, ".gcode"))
        filename.resize(filename.size() - 6);
    else if (ends_with(lower, ".bgcode"))
        filename.resize(filename.size() - 7);
    return filename + ".gcode.3mf";
}

std::string curl_escape_path_element(CURL *curl, const std::string &text)
{
    char *escaped = curl_easy_escape(curl, text.c_str(), int(text.size()));
    if (escaped == nullptr)
        return text;
    std::string out(escaped);
    curl_free(escaped);
    return out;
}

size_t discard_write_cb(char *ptr, size_t size, size_t nmemb, void *)
{
    return size * nmemb;
}

size_t string_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    std::string *out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

struct CurlProgressContext
{
    Http::ProgressFn progress_fn;
    std::string buffer;
    bool cancel = false;
};

int curl_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    CurlProgressContext *ctx = static_cast<CurlProgressContext*>(clientp);
    if (ctx->progress_fn) {
        Http::Progress progress(
            static_cast<size_t>(std::max<curl_off_t>(0, dltotal)),
            static_cast<size_t>(std::max<curl_off_t>(0, dlnow)),
            static_cast<size_t>(std::max<curl_off_t>(0, ultotal)),
            static_cast<size_t>(std::max<curl_off_t>(0, ulnow)),
            ctx->buffer);
        ctx->progress_fn(progress, ctx->cancel);
    }
    return ctx->cancel ? 1 : 0;
}

std::string curl_error(CURLcode code, const char *error_buffer)
{
    if (error_buffer != nullptr && error_buffer[0] != '\0')
        return error_buffer;
    return curl_easy_strerror(code);
}

void mqtt_append_remaining_length(std::vector<unsigned char> &packet, size_t length)
{
    do {
        unsigned char encoded = length % 128;
        length /= 128;
        if (length > 0)
            encoded |= 128;
        packet.push_back(encoded);
    } while (length > 0);
}

void mqtt_append_u16(std::vector<unsigned char> &packet, size_t value)
{
    packet.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    packet.push_back(static_cast<unsigned char>(value & 0xff));
}

void mqtt_append_utf8(std::vector<unsigned char> &packet, const std::string &value)
{
    mqtt_append_u16(packet, value.size());
    packet.insert(packet.end(), value.begin(), value.end());
}

std::string json_escape(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 32)
                out += ' ';
            else
                out += c;
            break;
        }
    }
    return out;
}

std::vector<unsigned char> mqtt_connect_packet(const std::string &client_id, const std::string &password)
{
    std::vector<unsigned char> variable;
    mqtt_append_utf8(variable, "MQTT");
    variable.push_back(4);    // MQTT 3.1.1
    variable.push_back(0xc2); // username, password, clean session
    mqtt_append_u16(variable, 60);
    mqtt_append_utf8(variable, client_id);
    mqtt_append_utf8(variable, BAMBU_LAN_USER);
    mqtt_append_utf8(variable, password);

    std::vector<unsigned char> packet;
    packet.push_back(0x10);
    mqtt_append_remaining_length(packet, variable.size());
    packet.insert(packet.end(), variable.begin(), variable.end());
    return packet;
}

std::vector<unsigned char> mqtt_publish_packet(const std::string &topic, const std::string &payload)
{
    std::vector<unsigned char> variable;
    mqtt_append_utf8(variable, topic);
    variable.insert(variable.end(), payload.begin(), payload.end());

    std::vector<unsigned char> packet;
    packet.push_back(0x30); // PUBLISH, QoS 0
    mqtt_append_remaining_length(packet, variable.size());
    packet.insert(packet.end(), variable.begin(), variable.end());
    return packet;
}

std::vector<unsigned char> mqtt_subscribe_packet(const std::string &topic)
{
    std::vector<unsigned char> variable;
    mqtt_append_u16(variable, 1);
    mqtt_append_utf8(variable, topic);
    variable.push_back(0);

    std::vector<unsigned char> packet;
    packet.push_back(0x82);
    mqtt_append_remaining_length(packet, variable.size());
    packet.insert(packet.end(), variable.begin(), variable.end());
    return packet;
}

bool mqtt_read_remaining_length(boost::asio::ssl::stream<boost::asio::ip::tcp::socket> &stream, size_t &length)
{
    length = 0;
    size_t multiplier = 1;
    for (int i = 0; i < 4; ++i) {
        unsigned char encoded = 0;
        boost::asio::read(stream, boost::asio::buffer(&encoded, 1));
        length += (encoded & 127) * multiplier;
        if ((encoded & 128) == 0)
            return true;
        multiplier *= 128;
    }
    return false;
}

void set_mqtt_read_timeout(boost::asio::ip::tcp::socket &socket, int seconds)
{
#ifdef __linux__
    timeval timeout {};
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#else
    (void)socket;
    (void)seconds;
#endif
}

std::string bool_json(bool value)
{
    return value ? "true" : "false";
}

std::string mqtt_start_payload(const std::string &remote_filename, const BambuStartOptions &options)
{
    const std::string escaped = json_escape(remote_filename);
    std::ostringstream payload;
    payload << "{\"print\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"project_file\","
            << "\"url\":\"file:///sdcard/" << escaped << "\","
            << "\"param\":\"Metadata/plate_1.gcode\","
            << "\"subtask_id\":\"0\","
            << "\"subtask_name\":\"" << escaped << "\","
            << "\"use_ams\":" << bool_json(options.use_ams) << ","
            << "\"timelapse\":" << bool_json(options.timelapse) << ","
            << "\"bed_leveling\":" << bool_json(options.bed_leveling) << ","
            << "\"flow_cali\":" << bool_json(options.flow_cali) << ","
            << "\"vibration_cali\":" << bool_json(options.vibration_cali) << ","
            << "\"layer_inspect\":" << bool_json(options.layer_inspect);
    if (!options.bed_type.empty())
        payload << ",\"bed_type\":\"" << json_escape(options.bed_type) << "\"";
    if (!options.ams_mapping.empty())
        payload << ",\"ams_mapping\":" << options.ams_mapping;
    payload
            << "}}";
    return payload.str();
}

std::string mqtt_print_command_payload(const std::string &command)
{
    std::ostringstream payload;
    payload << "{\"print\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"" << json_escape(command) << "\""
            << "}}";
    return payload.str();
}

std::string mqtt_pushing_command_payload(const std::string &command)
{
    std::ostringstream payload;
    payload << "{\"pushing\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"" << json_escape(command) << "\""
            << "}}";
    return payload.str();
}

std::string mqtt_gcode_payload(const std::string &gcode)
{
    std::ostringstream payload;
    payload << "{\"print\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"gcode_line\","
            << "\"param\":\"" << json_escape(gcode) << "\""
            << "}}";
    return payload.str();
}

std::string mqtt_int_print_payload(const std::string &command, const std::string &field, int value)
{
    std::ostringstream payload;
    payload << "{\"print\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"" << json_escape(command) << "\","
            << "\"" << json_escape(field) << "\":" << value
            << "}}";
    return payload.str();
}

std::string mqtt_nozzle_temp_payload(int temp)
{
    std::ostringstream payload;
    payload << "{\"print\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"set_nozzle_temp\","
            << "\"extruder_index\":0,"
            << "\"target_temp\":" << temp
            << "}}";
    return payload.str();
}

std::string mqtt_print_speed_payload(int speed_level)
{
    std::ostringstream payload;
    payload << "{\"print\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"print_speed\","
            << "\"param\":\"" << speed_level << "\""
            << "}}";
    return payload.str();
}

std::string mqtt_camera_bool_payload(const std::string &command, bool enabled)
{
    std::ostringstream payload;
    payload << "{\"camera\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"" << json_escape(command) << "\","
            << "\"control\":\"" << (enabled ? "enable" : "disable") << "\""
            << "}}";
    return payload.str();
}

std::string mqtt_camera_resolution_payload(const std::string &resolution)
{
    std::ostringstream payload;
    payload << "{\"camera\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"ipcam_resolution_set\","
            << "\"resolution\":\"" << json_escape(resolution) << "\""
            << "}}";
    return payload.str();
}

std::string mqtt_print_option_payload(const std::string &option, bool enabled)
{
    std::ostringstream payload;
    payload << "{\"print\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"print_option\","
            << "\"" << json_escape(option) << "\":" << bool_json(enabled)
            << "}}";
    return payload.str();
}

std::string mqtt_xcam_payload(const std::string &module, bool enabled, const std::string &sensitivity)
{
    std::ostringstream payload;
    payload << "{\"xcam\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"xcam_control_set\","
            << "\"module_name\":\"" << json_escape(module) << "\","
            << "\"control\":" << bool_json(enabled) << ","
            << "\"enable\":" << bool_json(enabled) << ","
            << "\"print_halt\":true";
    if (!sensitivity.empty())
        payload << ",\"halt_print_sensitivity\":\"" << json_escape(sensitivity) << "\"";
    payload << "}}";
    return payload.str();
}

std::string mqtt_ams_control_payload(const std::string &action)
{
    std::ostringstream payload;
    payload << "{\"print\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"ams_control\","
            << "\"param\":\"" << json_escape(action) << "\""
            << "}}";
    return payload.str();
}

std::string mqtt_ams_change_filament_payload(bool load, int ams_id, int slot_id, int current_temp, int target_temp)
{
    const int target = !load ? 255 : (ams_id < 16 ? ams_id * 4 + slot_id : ams_id);
    std::ostringstream payload;
    payload << "{\"print\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"ams_change_filament\","
            << "\"curr_temp\":" << current_temp << ","
            << "\"tar_temp\":" << target_temp << ","
            << "\"ams_id\":" << ams_id << ","
            << "\"target\":" << target << ","
            << "\"slot_id\":" << (!load ? 255 : slot_id)
            << "}}";
    return payload.str();
}

std::string mqtt_ams_get_rfid_payload(int ams_id, int slot_id)
{
    std::ostringstream payload;
    payload << "{\"print\":{"
            << "\"sequence_id\":\"0\","
            << "\"command\":\"ams_get_rfid\","
            << "\"ams_id\":" << ams_id << ","
            << "\"slot_id\":" << slot_id
            << "}}";
    return payload.str();
}

}

BambuLan::BambuLan(DynamicPrintConfig *config)
    : m_host(trim_host(config->opt_string("print_host")))
    , m_access_code(config->opt_string("printhost_apikey"))
    , m_serial(config->opt_string("printhost_user"))
{
}

wxString BambuLan::get_test_ok_msg() const
{
    return "Bambu Lab LAN connection established.";
}

wxString BambuLan::get_test_failed_msg(wxString &msg) const
{
    return wxString::Format("Could not connect to Bambu Lab printer: %s", msg);
}

bool BambuLan::test(wxString &msg) const
{
    if (m_host.empty()) {
        msg = "Printer hostname or IP is empty.";
        return false;
    }
    if (m_access_code.empty()) {
        msg = "LAN access code is empty. Put the printer LAN access code in the API key field.";
        return false;
    }

    std::string error;
    const bool ok = ftps_test(error);
    if (!ok)
        msg = wxString::FromUTF8(error.c_str());
    return ok;
}

bool BambuLan::ftps_test(std::string &error) const
{
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        error = "Could not initialize libcurl.";
        return false;
    }

    char error_buffer[CURL_ERROR_SIZE] = {};
    const std::string url = "ftps://" + m_host + ":" + std::to_string(BAMBU_FTPS_PORT) + "/sdcard/";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, BAMBU_LAN_USER);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_access_code.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write_cb);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        error = curl_error(code, error_buffer);
        return false;
    }
    return true;
}

bool BambuLan::ftps_upload(const fs::path &source_path, const std::string &remote_filename, ProgressFn progress_fn, std::string &error) const
{
    FILE *file = boost::nowide::fopen(source_path.string().c_str(), "rb");
    if (file == nullptr) {
        error = "Could not open generated G-code for upload.";
        return false;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        std::fclose(file);
        error = "Could not initialize libcurl.";
        return false;
    }

    char error_buffer[CURL_ERROR_SIZE] = {};
    const uintmax_t file_size = fs::file_size(source_path);
    const std::string url = "ftps://" + m_host + ":" + std::to_string(BAMBU_FTPS_PORT) + "/sdcard/" + curl_escape_path_element(curl, remote_filename);
    CurlProgressContext progress_ctx { progress_fn };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, BAMBU_LAN_USER);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_access_code.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, file);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(file_size));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write_cb);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_ctx);

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    std::fclose(file);

    if (progress_ctx.cancel) {
        error = "Upload cancelled.";
        return false;
    }
    if (code != CURLE_OK) {
        error = curl_error(code, error_buffer);
        return false;
    }
    return true;
}

bool BambuLan::list_sdcard(std::vector<std::string> &files, std::string &error) const
{
    files.clear();

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        error = "Could not initialize libcurl.";
        return false;
    }

    char error_buffer[CURL_ERROR_SIZE] = {};
    std::string listing;
    const std::string url = "ftps://" + m_host + ":" + std::to_string(BAMBU_FTPS_PORT) + "/sdcard/";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, BAMBU_LAN_USER);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_access_code.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, string_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &listing);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        error = curl_error(code, error_buffer);
        return false;
    }

    std::istringstream lines(listing);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim_copy(line);
        if (line.empty())
            continue;
        const size_t split = line.find_last_of(" \t");
        if (split != std::string::npos)
            line = trim_copy(line.substr(split + 1));
        if (!line.empty() && line != "." && line != "..")
            files.push_back(line);
    }
    return true;
}

bool BambuLan::delete_sdcard_file(const std::string &filename, std::string &error) const
{
    const std::string sanitized = sanitize_remote_filename(filename);
    if (sanitized.empty()) {
        error = "Filename is empty.";
        return false;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        error = "Could not initialize libcurl.";
        return false;
    }

    char error_buffer[CURL_ERROR_SIZE] = {};
    const std::string url = "ftps://" + m_host + ":" + std::to_string(BAMBU_FTPS_PORT) + "/sdcard/" + curl_escape_path_element(curl, sanitized);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, BAMBU_LAN_USER);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_access_code.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELE");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write_cb);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        error = curl_error(code, error_buffer);
        return false;
    }
    return true;
}

bool BambuLan::make_bambu_project_archive(const fs::path &source_path, fs::path &archive_path, std::string &error) const
{
    archive_path = fs::temp_directory_path() / fs::unique_path(".PrusaSlicer.bambu.%%%%-%%%%-%%%%-%%%%.gcode.3mf");

    mz_zip_archive archive;
    std::memset(&archive, 0, sizeof(archive));

    if (!mz_zip_writer_init_file(&archive, archive_path.string().c_str(), 0)) {
        error = "Could not create temporary Bambu .gcode.3mf archive.";
        return false;
    }

    bool ok = mz_zip_writer_add_file(&archive, "Metadata/plate_1.gcode", source_path.string().c_str(), nullptr, 0, MZ_BEST_SPEED);
    if (ok)
        ok = mz_zip_writer_finalize_archive(&archive);
    if (!mz_zip_writer_end(&archive))
        ok = false;

    if (!ok) {
        error = "Could not package G-code as a Bambu .gcode.3mf archive.";
        boost::system::error_code ec;
        fs::remove(archive_path, ec);
        return false;
    }

    return true;
}

bool BambuLan::mqtt_publish_json(const std::string &payload, std::string &error) const
{
    if (m_serial.empty()) {
        error = "Printer serial/device ID is empty. Put the X1C serial number in the Username field.";
        return false;
    }

    try {
        namespace asio = boost::asio;
        using asio::ip::tcp;

        asio::io_context io;
        asio::ssl::context ssl_context(asio::ssl::context::tls_client);
        ssl_context.set_verify_mode(asio::ssl::verify_none);

        tcp::resolver resolver(io);
        boost::asio::ssl::stream<tcp::socket> stream(io, ssl_context);
        asio::connect(stream.next_layer(), resolver.resolve(m_host, std::to_string(BAMBU_MQTT_PORT)));
        set_mqtt_read_timeout(stream.next_layer(), 8);
        stream.handshake(asio::ssl::stream_base::client);

        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string client_id = "prusaslicer-" + std::to_string(now);
        const std::vector<unsigned char> connect = mqtt_connect_packet(client_id, m_access_code);
        asio::write(stream, asio::buffer(connect));

        unsigned char connack[4] = {};
        asio::read(stream, asio::buffer(connack));
        if (connack[0] != 0x20 || connack[1] != 0x02 || connack[3] != 0x00) {
            error = "Bambu MQTT login failed.";
            return false;
        }

        const std::string topic = "device/" + m_serial + "/request";
        const std::vector<unsigned char> publish = mqtt_publish_packet(topic, payload);
        asio::write(stream, asio::buffer(publish));

        const unsigned char disconnect[] = { 0xe0, 0x00 };
        asio::write(stream, asio::buffer(disconnect));
        boost::system::error_code ignored;
        stream.shutdown(ignored);
        stream.next_layer().close(ignored);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

bool BambuLan::mqtt_start_print(const std::string &remote_filename, const std::string &print_options, std::string &error) const
{
    const BambuStartOptions start_options = parse_bambu_start_options(print_options);
    return mqtt_publish_json(mqtt_start_payload(remote_filename, start_options), error);
}

bool BambuLan::start_sdcard_file(const std::string &filename, const std::string &print_options, std::string &error) const
{
    return mqtt_start_print(sanitize_remote_filename(filename), print_options, error);
}

bool BambuLan::send_print_command(const std::string &command, std::string &error) const
{
    return mqtt_publish_json(mqtt_print_command_payload(command), error);
}

bool BambuLan::send_pushing_command(const std::string &command, std::string &error) const
{
    return mqtt_publish_json(mqtt_pushing_command_payload(command), error);
}

bool BambuLan::send_gcode_line(const std::string &gcode, std::string &error) const
{
    return mqtt_publish_json(mqtt_gcode_payload(gcode), error);
}

bool BambuLan::set_bed_temp(int temp, std::string &error) const
{
    return mqtt_publish_json(mqtt_int_print_payload("set_bed_temp", "temp", temp), error);
}

bool BambuLan::set_nozzle_temp(int temp, std::string &error) const
{
    return mqtt_publish_json(mqtt_nozzle_temp_payload(temp), error);
}

bool BambuLan::set_chamber_temp(int temp, std::string &error) const
{
    return mqtt_publish_json(mqtt_int_print_payload("set_ctt", "ctt_val", temp), error);
}

bool BambuLan::set_print_speed(int speed_level, std::string &error) const
{
    speed_level = std::clamp(speed_level, 1, 4);
    return mqtt_publish_json(mqtt_print_speed_payload(speed_level), error);
}

bool BambuLan::set_camera_recording(bool enabled, std::string &error) const
{
    return mqtt_publish_json(mqtt_camera_bool_payload("ipcam_record_set", enabled), error);
}

bool BambuLan::set_camera_timelapse(bool enabled, std::string &error) const
{
    return mqtt_publish_json(mqtt_camera_bool_payload("ipcam_timelapse", enabled), error);
}

bool BambuLan::set_camera_resolution(const std::string &resolution, std::string &error) const
{
    return mqtt_publish_json(mqtt_camera_resolution_payload(resolution), error);
}

bool BambuLan::set_print_option(const std::string &option, bool enabled, std::string &error) const
{
    return mqtt_publish_json(mqtt_print_option_payload(option, enabled), error);
}

bool BambuLan::set_xcam_module(const std::string &module, bool enabled, const std::string &sensitivity, std::string &error) const
{
    return mqtt_publish_json(mqtt_xcam_payload(module, enabled, sensitivity), error);
}

bool BambuLan::send_ams_control(const std::string &action, std::string &error) const
{
    return mqtt_publish_json(mqtt_ams_control_payload(action), error);
}

bool BambuLan::ams_change_filament(bool load, int ams_id, int slot_id, int current_temp, int target_temp, std::string &error) const
{
    return mqtt_publish_json(mqtt_ams_change_filament_payload(load, ams_id, slot_id, current_temp, target_temp), error);
}

bool BambuLan::ams_refresh_rfid(int ams_id, int slot_id, std::string &error) const
{
    return mqtt_publish_json(mqtt_ams_get_rfid_payload(ams_id, slot_id), error);
}

bool BambuLan::ams_calibrate(int ams_id, std::string &error) const
{
    std::ostringstream gcode;
    gcode << "M620 C" << ams_id << "\n";
    return send_gcode_line(gcode.str(), error);
}

bool BambuLan::request_status(std::string &status_json, std::string &error) const
{
    status_json.clear();
    if (m_serial.empty()) {
        error = "Printer serial/device ID is empty. Put the X1C serial number in the Username field.";
        return false;
    }

    try {
        namespace asio = boost::asio;
        using asio::ip::tcp;

        asio::io_context io;
        asio::ssl::context ssl_context(asio::ssl::context::tls_client);
        ssl_context.set_verify_mode(asio::ssl::verify_none);

        tcp::resolver resolver(io);
        boost::asio::ssl::stream<tcp::socket> stream(io, ssl_context);
        asio::connect(stream.next_layer(), resolver.resolve(m_host, std::to_string(BAMBU_MQTT_PORT)));
        set_mqtt_read_timeout(stream.next_layer(), 8);
        stream.handshake(asio::ssl::stream_base::client);

        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string client_id = "prusaslicer-status-" + std::to_string(now);
        const std::vector<unsigned char> connect = mqtt_connect_packet(client_id, m_access_code);
        asio::write(stream, asio::buffer(connect));

        unsigned char connack[4] = {};
        asio::read(stream, asio::buffer(connack));
        if (connack[0] != 0x20 || connack[1] != 0x02 || connack[3] != 0x00) {
            error = "Bambu MQTT login failed.";
            return false;
        }

        const std::string report_topic = "device/" + m_serial + "/report";
        asio::write(stream, asio::buffer(mqtt_subscribe_packet(report_topic)));
        asio::write(stream, asio::buffer(mqtt_publish_packet("device/" + m_serial + "/request", mqtt_pushing_command_payload("pushall"))));

        for (int packet_index = 0; packet_index < 20; ++packet_index) {
            unsigned char header = 0;
            boost::system::error_code ec;
            asio::read(stream, asio::buffer(&header, 1), ec);
            if (ec) {
                error = ec.message();
                return false;
            }

            size_t remaining_length = 0;
            if (!mqtt_read_remaining_length(stream, remaining_length)) {
                error = "Invalid MQTT packet length.";
                return false;
            }
            std::vector<unsigned char> body(remaining_length);
            if (!body.empty())
                asio::read(stream, asio::buffer(body));

            if ((header & 0xf0) != 0x30 || body.size() < 2)
                continue;

            const size_t topic_len = (size_t(body[0]) << 8) | size_t(body[1]);
            if (body.size() < 2 + topic_len)
                continue;
            const std::string topic(reinterpret_cast<const char*>(body.data() + 2), topic_len);
            if (topic != report_topic)
                continue;
            status_json.assign(reinterpret_cast<const char*>(body.data() + 2 + topic_len), body.size() - 2 - topic_len);
            break;
        }

        const unsigned char disconnect[] = { 0xe0, 0x00 };
        boost::system::error_code ignored;
        asio::write(stream, asio::buffer(disconnect), ignored);
        stream.shutdown(ignored);
        stream.next_layer().close(ignored);

        if (status_json.empty()) {
            error = "No status report received from printer.";
            return false;
        }
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

bool BambuLan::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    if (m_host.empty()) {
        error_fn("Printer hostname or IP is empty.");
        return false;
    }
    if (m_access_code.empty()) {
        error_fn("LAN access code is empty. Put the printer LAN access code in the API key field.");
        return false;
    }

    std::string remote_filename = upload_data.upload_path.filename().string();
    if (remote_filename.empty())
        remote_filename = upload_data.source_path.filename().string();
    remote_filename = sanitize_remote_filename(remote_filename);

    std::string error;
    fs::path source_path = upload_data.source_path;
    fs::path bambu_archive_path;

    if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
        remote_filename = bambu_project_filename(remote_filename);
        info_fn("Bambu Lab LAN", "Packaging G-code for Bambu print start");
        if (!make_bambu_project_archive(upload_data.source_path, bambu_archive_path, error)) {
            error_fn(wxString::FromUTF8(error.c_str()));
            return false;
        }
        source_path = bambu_archive_path;
    }

    info_fn("Bambu Lab LAN", "Uploading file to printer SD card");
    if (!ftps_upload(source_path, remote_filename, progress_fn, error)) {
        if (!bambu_archive_path.empty()) {
            boost::system::error_code ec;
            fs::remove(bambu_archive_path, ec);
        }
        error_fn(wxString::FromUTF8(error.c_str()));
        return false;
    }

    if (!bambu_archive_path.empty()) {
        boost::system::error_code ec;
        fs::remove(bambu_archive_path, ec);
    }

    if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
        info_fn("Bambu Lab LAN", "Starting print");
        if (!mqtt_start_print(remote_filename, upload_data.data_json, error)) {
            error_fn(wxString::FromUTF8(error.c_str()));
            return false;
        }
    }

    info_fn("Bambu Lab LAN", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "Uploaded and start command sent" : "Uploaded");
    return true;
}

}
