#pragma once
// Minimal HTTPS downloads for the updater, using what each OS already has:
//   Windows: WinHTTP (system proxy settings, no console window)
//   macOS / Linux: the curl command (/usr/bin/curl on macOS; any curl on PATH on Linux)
// Downloads run on the calling thread; call from a worker thread.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace supersaga::update {

struct HttpOptions {
    std::string user_agent = "SuperSaga";
    std::string accept;            // Accept header (empty: none)
    bool allow_plain_http = false; // only for local testing feeds
    const std::atomic<bool>* cancel = nullptr;
    // Called periodically with bytes written so far and the expected total (0 if unknown).
    std::function<void(std::uint64_t done, std::uint64_t total)> progress;
    std::uint64_t expected_size = 0;
    int* status_out = nullptr;     // receives the HTTP status of a failed request (0 if none)
};

// Downloads `url` into `file` (overwritten). Follows redirects. HTTP errors (4xx/5xx) fail.
bool http_download(const std::string& url, const std::filesystem::path& file, const HttpOptions& opt,
                   std::string* error);

// Downloads `url` into memory (small replies only, such as the release JSON or checksums).
bool http_get(const std::string& url, std::string* body, const HttpOptions& opt, std::string* error);

}  // namespace supersaga::update
