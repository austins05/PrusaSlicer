///|/ Copyright (c) Prusa Research 2018 - 2023
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BambuLan.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>

#include <curl/curl.h>
#include <wx/string.h>

#include "libslic3r/PrintConfig.hpp"
#include "miniz.h"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace {

constexpr const char *BAMBU_LAN_USER = "bblp";
constexpr unsigned BAMBU_FTPS_PORT = 990;
constexpr unsigned BAMBU_MQTT_PORT = 8883;

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

std::string sanitize_remote_filename(std::string filename)
{
    if (filename.empty())
        filename = "prusaslicer.gcode";
    for (char &c : filename)
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32)
            c = '_';
    return filename;
}

std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
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

std::string mqtt_start_payload(const std::string &remote_filename)
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
            << "\"use_ams\":false,"
            << "\"timelapse\":false,"
            << "\"bed_leveling\":true,"
            << "\"flow_cali\":false,"
            << "\"vibration_cali\":false,"
            << "\"layer_inspect\":false"
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

bool BambuLan::mqtt_start_print(const std::string &remote_filename, std::string &error) const
{
    if (m_serial.empty()) {
        error = "Printer serial/device ID is empty. Put the X1C serial number in the Username field to start prints over LAN.";
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
        const std::string payload = mqtt_start_payload(remote_filename);
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
        if (!mqtt_start_print(remote_filename, error)) {
            error_fn(wxString::FromUTF8(error.c_str()));
            return false;
        }
    }

    info_fn("Bambu Lab LAN", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "Uploaded and start command sent" : "Uploaded");
    return true;
}

}
