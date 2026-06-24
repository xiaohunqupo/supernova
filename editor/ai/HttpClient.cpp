#include "HttpClient.h"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifndef DORIAX_AI_USE_CURL_CLI
#include <curl/curl.h>
#endif

namespace fs = std::filesystem;

namespace doriax::editor::ai {

namespace {

#ifndef DORIAX_AI_USE_CURL_CLI

size_t writeStringCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* text = static_cast<std::string*>(userdata);
    const size_t total = size * nmemb;
    text->append(ptr, total);
    return total;
}

size_t writeFileCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* file = static_cast<std::ofstream*>(userdata);
    const size_t total = size * nmemb;
    file->write(ptr, static_cast<std::streamsize>(total));
    return file->good() ? total : 0;
}

int progressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* cancel = static_cast<const std::atomic<bool>*>(clientp);
    return (cancel && cancel->load()) ? 1 : 0;
}

curl_slist* buildHeaders(const std::vector<std::string>& headers) {
    curl_slist* list = nullptr;
    for (const auto& header : headers) {
        list = curl_slist_append(list, header.c_str());
    }
    return list;
}

HttpResponse configureAndPerform(CURL* curl,
                                 const HttpRequest& request,
                                 void* writeData,
                                 size_t (*writeCallback)(char*, size_t, size_t, void*),
                                 const std::atomic<bool>* cancel) {
    HttpResponse response;
    curl_slist* headers = buildHeaders(request.headers);

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DoriaxEditorAI/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, writeData);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    if (request.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    }

    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);

    if (result != CURLE_OK) {
        response.error = curl_easy_strerror(result);
    }

    if (headers) {
        curl_slist_free_all(headers);
    }
    return response;
}

#else

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\"'\"'";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

fs::path tempPath(const std::string& name) {
    static std::atomic<uint64_t> counter{0};
    return fs::temp_directory_path() / ("doriax_ai_" + name + "_" +
        std::to_string(static_cast<unsigned long long>(std::time(nullptr))) + "_" +
        std::to_string(counter.fetch_add(1)));
}

bool readFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

long readStatus(const fs::path& path) {
    std::string text;
    if (!readFile(path, text)) return 0;
    try {
        return std::stol(text);
    } catch (...) {
        return 0;
    }
}

// Writes the URL and headers into a curl --config file (mode 0600) so secrets
// (API keys in Authorization/x-api-key headers, or the Gemini key in the URL
// query string) never appear in argv, where other local processes could read
// them via /proc. The config path itself is not sensitive.
bool writeCurlConfig(const fs::path& path,
                     const std::string& url,
                     const std::vector<std::string>& headers) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return false;
    }
    std::error_code ec;
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);

    auto escape = [](const std::string& value) {
        std::string escaped;
        for (char c : value) {
            if (c == '\\' || c == '"') escaped += '\\';
            escaped += c;
        }
        return escaped;
    };

    out << "url = \"" << escape(url) << "\"\n";
    for (const auto& header : headers) {
        out << "header = \"" << escape(header) << "\"\n";
    }
    return out.good();
}

std::string buildCurlBase(long timeoutSeconds, const fs::path& configFile) {
    std::ostringstream cmd;
    cmd << "curl -L -sS --max-redirs 5 --max-time " << timeoutSeconds
        << " -A " << shellQuote("DoriaxEditorAI/1.0")
        << " --config " << shellQuote(configFile.string())
        << " -w " << shellQuote("%{http_code}")
        << " -o ";
    return cmd.str();
}

HttpResponse runCurlCommand(const std::string& cmd,
                            const fs::path& bodyFile,
                            const fs::path& statusFile) {
    HttpResponse response;
    int rc = std::system(cmd.c_str());
    response.status = readStatus(statusFile);
    readFile(bodyFile, response.body);
    if (rc != 0) {
        response.error = "curl command failed";
    }
    std::error_code ec;
    fs::remove(statusFile, ec);
    return response;
}

#endif

} // namespace

HttpResponse HttpClient::send(const HttpRequest& request, const std::atomic<bool>* cancel) const {
    if (cancel && cancel->load()) {
        return {0, {}, "Request cancelled"};
    }

#ifndef DORIAX_AI_USE_CURL_CLI
    CURL* curl = curl_easy_init();
    if (!curl) {
        return {0, {}, "Failed to initialize libcurl"};
    }

    std::string body;
    HttpResponse response = configureAndPerform(curl, request, &body, writeStringCallback, cancel);
    response.body = std::move(body);
    curl_easy_cleanup(curl);
    return response;
#else
    fs::path bodyFile = tempPath("response");
    fs::path statusFile = tempPath("status");
    fs::path requestBodyFile = tempPath("request");
    fs::path configFile = tempPath("config");

    if (!writeCurlConfig(configFile, request.url, request.headers)) {
        return {0, {}, "Failed to prepare curl request"};
    }

    std::ostringstream cmd;
    cmd << buildCurlBase(request.timeoutSeconds, configFile)
        << shellQuote(bodyFile.string());
    if (request.method == "POST") {
        std::ofstream bodyOut(requestBodyFile, std::ios::binary | std::ios::trunc);
        bodyOut << request.body;
        bodyOut.close();
        cmd << " -X POST --data-binary @" << shellQuote(requestBodyFile.string());
    }
    cmd << " > " << shellQuote(statusFile.string());

    HttpResponse response = runCurlCommand(cmd.str(), bodyFile, statusFile);
    std::error_code ec;
    fs::remove(bodyFile, ec);
    fs::remove(requestBodyFile, ec);
    fs::remove(configFile, ec);
    if (cancel && cancel->load()) {
        response.error = "Request cancelled";
    }
    return response;
#endif
}

HttpResponse HttpClient::get(const std::string& url,
                             const std::vector<std::string>& headers,
                             const std::atomic<bool>* cancel) const {
    HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.headers = headers;
    return send(request, cancel);
}

HttpResponse HttpClient::downloadToFile(const std::string& url,
                                        const fs::path& destination,
                                        const std::vector<std::string>& headers,
                                        const std::atomic<bool>* cancel) const {
    if (cancel && cancel->load()) {
        return {0, {}, "Request cancelled"};
    }
    if (destination.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(destination.parent_path(), ec);
        if (ec) {
            return {0, {}, "Failed to create destination directory: " + ec.message()};
        }
    }

#ifndef DORIAX_AI_USE_CURL_CLI
    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    if (!file) {
        return {0, {}, "Failed to open destination file"};
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return {0, {}, "Failed to initialize libcurl"};
    }

    HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.headers = headers;
    request.timeoutSeconds = 300;

    HttpResponse response = configureAndPerform(curl, request, &file, writeFileCallback, cancel);
    curl_easy_cleanup(curl);
    file.close();
#else
    fs::path statusFile = tempPath("status");
    fs::path configFile = tempPath("config");
    if (!writeCurlConfig(configFile, url, headers)) {
        return {0, {}, "Failed to prepare curl request"};
    }
    std::ostringstream cmd;
    cmd << buildCurlBase(300, configFile)
        << shellQuote(destination.string())
        << " > " << shellQuote(statusFile.string());
    int rc = std::system(cmd.str().c_str());
    HttpResponse response;
    response.status = readStatus(statusFile);
    if (rc != 0) {
        response.error = "curl command failed";
    }
    std::error_code statusEc;
    fs::remove(statusFile, statusEc);
    fs::remove(configFile, statusEc);
#endif

    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        std::error_code ec;
        fs::remove(destination, ec);
    }
    if (cancel && cancel->load()) {
        response.error = "Request cancelled";
    }
    return response;
}

std::string HttpClient::urlEncode(const std::string& value) {
#ifndef DORIAX_AI_USE_CURL_CLI
    CURL* curl = curl_easy_init();
    if (!curl) {
        return value;
    }
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    std::string out = encoded ? encoded : value;
    if (encoded) {
        curl_free(encoded);
    }
    curl_easy_cleanup(curl);
    return out;
#else
    std::ostringstream encoded;
    encoded << std::hex << std::uppercase;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << static_cast<char>(c);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return encoded.str();
#endif
}

} // namespace doriax::editor::ai
