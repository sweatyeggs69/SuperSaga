#include "app/http.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace supersaga::http {
namespace {

struct Job {
    std::string url, post, content_type;
    Callback done;
    Response response;
};

// Lives for the whole process (never freed) so detached workers can never outlive it.
struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> queue;
    std::vector<Job> finished;
    std::string user_agent = "SuperSaga";
    int workers = 0, idle = 0, pending = 0;
};
State& state() {
    static State* s = new State;
    return *s;
}
constexpr int kMaxWorkers = 3;

#ifdef _WIN32
// ---- WinHTTP ---------------------------------------------------------------------------------

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

bool backend_ready() { return true; }

void perform(Job& job, const std::string& agent) {
    Response& r = job.response;
    std::wstring url = widen(job.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = DWORD(std::size(path));
    wchar_t extra[4096] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = DWORD(std::size(extra));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        r.error = "Bad URL";
        return;
    }
    std::wstring object = std::wstring(path) + extra;
    HINTERNET session = WinHttpOpen(widen(agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)  // older Windows without the automatic proxy option
        session = WinHttpOpen(widen(agent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET req = connect ? WinHttpOpenRequest(connect, job.post.empty() ? L"GET" : L"POST", object.c_str(),
                                                 nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 secure ? WINHTTP_FLAG_SECURE : 0)
                            : nullptr;
    if (req) WinHttpSetTimeouts(req, 10000, 10000, 20000, 30000);
    std::wstring headers;
    if (!job.post.empty() && !job.content_type.empty()) headers = L"Content-Type: " + widen(job.content_type);
    bool ok = req && WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                        headers.empty() ? 0 : DWORD(-1L),
                                        job.post.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)job.post.data(),
                                        DWORD(job.post.size()), DWORD(job.post.size()), 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        r.status = int(code);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            size_t at = r.body.size();
            r.body.resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, r.body.data() + at, avail, &got)) { r.body.resize(at); break; }
            r.body.resize(at + got);
        }
    } else {
        r.error = "Could not reach the server (WinHTTP error " + std::to_string(GetLastError()) + ")";
    }
    if (req) WinHttpCloseHandle(req);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
}
const char* backend() { return "WinHTTP"; }

#else
// ---- libcurl, loaded at run time ---------------------------------------------------------------
// Only the stable easy interface is used; option numbers are part of libcurl's ABI.

using CURL = void;
struct curl_slist;
enum : int {
    kOptWriteData = 10001, kOptUrl = 10002, kOptPostFields = 10015, kOptUserAgent = 10018,
    kOptHttpHeader = 10023, kOptWriteFunction = 20011, kOptFollowLocation = 52, kOptPostFieldSize = 60,
    kOptTimeout = 13, kOptConnectTimeout = 78, kOptNoSignal = 99, kOptAcceptEncoding = 10102,
};
constexpr int kInfoResponseCode = 0x200000 + 2;

struct Curl {
    bool loaded = false;
    int (*global_init)(long) = nullptr;
    CURL* (*easy_init)() = nullptr;
    int (*easy_setopt)(CURL*, int, ...) = nullptr;
    int (*easy_perform)(CURL*) = nullptr;
    int (*easy_getinfo)(CURL*, int, ...) = nullptr;
    void (*easy_cleanup)(CURL*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
    curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
    void (*slist_free_all)(curl_slist*) = nullptr;
};

Curl& curl() {
    static Curl c = [] {
        Curl c;
#ifdef __APPLE__
        const char* names[] = {"/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib"};
#else
        const char* names[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
#endif
        void* lib = nullptr;
        for (const char* n : names)
            if ((lib = dlopen(n, RTLD_NOW | RTLD_LOCAL))) break;
        if (!lib) return c;
        auto sym = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            return fn != nullptr;
        };
        c.loaded = sym(c.global_init, "curl_global_init") && sym(c.easy_init, "curl_easy_init") &&
                   sym(c.easy_setopt, "curl_easy_setopt") && sym(c.easy_perform, "curl_easy_perform") &&
                   sym(c.easy_getinfo, "curl_easy_getinfo") && sym(c.easy_cleanup, "curl_easy_cleanup") &&
                   sym(c.easy_strerror, "curl_easy_strerror") && sym(c.slist_append, "curl_slist_append") &&
                   sym(c.slist_free_all, "curl_slist_free_all");
        if (c.loaded) c.global_init(3);  // CURL_GLOBAL_DEFAULT, on the main thread before any worker
        return c;
    }();
    return c;
}

bool backend_ready() { return curl().loaded; }

size_t on_write(char* data, size_t size, size_t n, void* user) {
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

void perform(Job& job, const std::string& agent) {
    Curl& c = curl();
    Response& r = job.response;
    CURL* h = c.easy_init();
    if (!h) {
        r.error = "Could not start a network request";
        return;
    }
    curl_slist* headers = nullptr;
    c.easy_setopt(h, kOptUrl, job.url.c_str());
    c.easy_setopt(h, kOptUserAgent, agent.c_str());
    c.easy_setopt(h, kOptWriteFunction, &on_write);
    c.easy_setopt(h, kOptWriteData, &r.body);
    c.easy_setopt(h, kOptFollowLocation, 1L);
    c.easy_setopt(h, kOptNoSignal, 1L);
    c.easy_setopt(h, kOptConnectTimeout, 10L);
    c.easy_setopt(h, kOptTimeout, 30L);
    c.easy_setopt(h, kOptAcceptEncoding, "");  // whatever the library supports
    if (!job.post.empty()) {
        c.easy_setopt(h, kOptPostFields, job.post.c_str());
        c.easy_setopt(h, kOptPostFieldSize, long(job.post.size()));
        if (!job.content_type.empty()) {
            std::string ct = "Content-Type: " + job.content_type;
            headers = c.slist_append(headers, ct.c_str());
            c.easy_setopt(h, kOptHttpHeader, headers);
        }
    }
    int rc = c.easy_perform(h);
    if (rc == 0) {
        long code = 0;
        c.easy_getinfo(h, kInfoResponseCode, &code);
        r.status = int(code);
    } else {
        r.error = std::string("Could not reach the server (") + c.easy_strerror(rc) + ")";
        r.body.clear();
    }
    if (headers) c.slist_free_all(headers);
    c.easy_cleanup(h);
}
const char* backend() { return "libcurl"; }
#endif

void worker() {
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    for (;;) {
        ++s.idle;
        s.wake.wait(lock, [&] { return !s.queue.empty(); });
        --s.idle;
        Job job = std::move(s.queue.front());
        s.queue.pop_front();
        std::string agent = s.user_agent;
        lock.unlock();
        perform(job, agent);
        lock.lock();
        s.finished.push_back(std::move(job));
    }
}

}  // namespace

void set_user_agent(std::string agent) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.user_agent = std::move(agent);
}

bool available() { return backend_ready(); }
const char* backend_name() { return backend_ready() ? backend() : "none"; }

void request(std::string url, std::string post_data, std::string content_type, Callback done) {
    State& s = state();
    Job job{std::move(url), std::move(post_data), std::move(content_type), std::move(done), {}};
    if (!backend_ready()) {  // still answered through pump(), never re-entrantly
        job.response.error = "No network library was found on this system";
        std::lock_guard<std::mutex> lock(s.mutex);
        ++s.pending;
        s.finished.push_back(std::move(job));
        return;
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.pending;
    s.queue.push_back(std::move(job));
    if (s.idle == 0 && s.workers < kMaxWorkers) {
        ++s.workers;
        std::thread(worker).detach();
    }
    s.wake.notify_one();
}

void pump() {
    State& s = state();
    std::vector<Job> done;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        done.swap(s.finished);
    }
    for (Job& j : done) {
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            --s.pending;
        }
        if (j.done) j.done(std::move(j.response));
    }
}

int pending() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.pending;
}

}  // namespace supersaga::http
