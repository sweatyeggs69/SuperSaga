#pragma once
// Minimal asynchronous HTTPS client for RetroAchievements (API calls and badge images).
// Requests run on background threads; their callbacks run on the thread that calls pump().
//
// Backends: libcurl loaded at run time on macOS (system library) and Linux (libcurl.so.4), so
// nothing extra is linked or shipped; WinHTTP on Windows. Without a backend every request fails
// with an error and the rest of the app keeps working.

#include <functional>
#include <string>

namespace supersaga::http {

struct Response {
    int status = 0;     // HTTP status code; 0 when the request did not reach the server
    std::string body;
    std::string error;  // transport error (empty on success)
};
using Callback = std::function<void(Response)>;

// Sent with every request.
void set_user_agent(std::string agent);
// Starts a request. post_data empty means GET, otherwise POST with the given content type.
void request(std::string url, std::string post_data, std::string content_type, Callback done);
// Runs the callbacks of finished requests. Call once per frame on the main thread.
void pump();
// Requests started and not yet delivered by pump().
int pending();
// True when an HTTP backend is present on this system.
bool available();
// Name of the backend, for diagnostics ("libcurl", "WinHTTP" or "none").
const char* backend_name();

}  // namespace supersaga::http
