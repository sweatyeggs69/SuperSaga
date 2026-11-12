#include "update/update_core.h"

#include "app/i18n.h"  // N_() marks the messages the settings page translates

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>

#if SUPERSAGA_HAVE_ZLIB
#include <zlib.h>
#endif

namespace supersaga::update {
namespace fs = std::filesystem;

// ---- versions -------------------------------------------------------------------------------

std::optional<Version> parse_version(const std::string& text) {
    std::string s = text;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i;
    Version v;
    int* parts[3] = {&v.major, &v.minor, &v.patch};
    for (int k = 0; k < 3; ++k) {
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
            if (k == 0) return std::nullopt;
            break;
        }
        long n = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            n = n * 10 + (s[i++] - '0');
            if (n > 1000000) return std::nullopt;
        }
        *parts[k] = int(n);
        if (k < 2 && i < s.size() && s[i] == '.') ++i;
        else break;
    }
    if (i < s.size() && s[i] == '-') v.pre = s.substr(i + 1);
    size_t plus = v.pre.find('+');  // build metadata is ignored
    if (plus != std::string::npos) v.pre.resize(plus);
    return v;
}

int compare_versions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre == b.pre) return 0;
    if (a.pre.empty()) return 1;  // 1.0.0 > 1.0.0-rc.1
    if (b.pre.empty()) return -1;
    return a.pre < b.pre ? -1 : 1;
}

std::string to_string(const Version& v) {
    std::string s = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    if (!v.pre.empty()) s += "-" + v.pre;
    return s;
}

// ---- a small JSON reader --------------------------------------------------------------------

namespace {

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json* get(const std::string& key) const {
        for (const auto& [k, v] : o)
            if (k == key) return &v;
        return nullptr;
    }
    std::string str(const std::string& key) const {
        const Json* v = get(key);
        return v && v->type == String ? v->s : std::string();
    }
    bool boolean(const std::string& key) const {
        const Json* v = get(key);
        return v && v->type == Bool && v->b;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& t) : t_(t) {}
    bool parse(Json& out) {
        if (!value(out, 0)) return false;
        ws();
        return i_ == t_.size();
    }

private:
    const std::string& t_;
    size_t i_ = 0;

    void ws() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
    }
    bool lit(const char* w) {
        size_t n = std::strlen(w);
        if (t_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    static void put_utf8(std::string& s, std::uint32_t c) {
        if (c < 0x80) s += char(c);
        else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F));
        } else {
            s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 0x3F));
            s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F));
        }
    }
    bool hex4(std::uint32_t& c) {
        if (i_ + 4 > t_.size()) return false;
        c = 0;
        for (int k = 0; k < 4; ++k) {
            char h = t_[i_++];
            c <<= 4;
            if (h >= '0' && h <= '9') c |= std::uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f') c |= std::uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') c |= std::uint32_t(h - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& s) {
        if (i_ >= t_.size() || t_[i_] != '"') return false;
        ++i_;
        while (i_ < t_.size()) {
            char ch = t_[i_++];
            if (ch == '"') return true;
            if (ch != '\\') { s += ch; continue; }
            if (i_ >= t_.size()) return false;
            char e = t_[i_++];
            switch (e) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': {
                    std::uint32_t c;
                    if (!hex4(c)) return false;
                    if (c >= 0xD800 && c < 0xDC00 && t_.compare(i_, 2, "\\u") == 0) {
                        i_ += 2;
                        std::uint32_t lo;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        else { put_utf8(s, 0xFFFD); c = lo; }
                    }
                    if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
                    put_utf8(s, c);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Json& v, int depth) {
        if (depth > 64) return false;
        ws();
        if (i_ >= t_.size()) return false;
        char c = t_[i_];
        if (c == '{') {
            v.type = Json::Object;
            ++i_;
            ws();
            if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i_ >= t_.size() || t_[i_] != ':') return false;
                ++i_;
                Json item;
                if (!value(item, depth + 1)) return false;
                v.o.emplace_back(std::move(key), std::move(item));
                ws();
                if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
                if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = Json::Array;
            ++i_;
            ws();
            if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
            for (;;) {
                Json item;
                if (!value(item, depth + 1)) return false;
                v.a.push_back(std::move(item));
                ws();
                if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
                if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = Json::String; return string(v.s); }
        if (lit("true")) { v.type = Json::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Json::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Json::Null; return true; }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = i_;
            ++i_;
            while (i_ < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[i_])) || t_[i_] == '.' ||
                                      t_[i_] == 'e' || t_[i_] == 'E' || t_[i_] == '+' || t_[i_] == '-'))
                ++i_;
            v.type = Json::Number;
            v.n = std::strtod(t_.substr(start, i_ - start).c_str(), nullptr);
            return true;
        }
        return false;
    }
};

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

}  // namespace

const Asset* Release::find(const std::string& n) const {
    for (const Asset& a : assets)
        if (a.name == n) return &a;
    return nullptr;
}

std::optional<Release> parse_release_json(const std::string& json, std::string* error) {
    Json root;
    if (!JsonParser(json).parse(root) || root.type != Json::Object) {
        if (error) *error = N_("The update server sent a reply SuperSaga couldn't read.");
        return std::nullopt;
    }
    Release r;
    r.tag = root.str("tag_name");
    r.name = root.str("name");
    r.notes = root.str("body");
    r.page_url = root.str("html_url");
    r.draft = root.boolean("draft");
    r.prerelease = root.boolean("prerelease");
    if (r.tag.empty()) {
        std::string msg = root.str("message");  // e.g. {"message": "Not Found"} when there's no release yet
        if (error) *error = msg == "Not Found" ? N_("No releases have been published yet.")
                            : msg.empty()      ? N_("The update server's reply had no version in it.")
                                               : "The update server said: " + msg;
        return std::nullopt;
    }
    if (const Json* assets = root.get("assets"); assets && assets->type == Json::Array) {
        for (const Json& a : assets->a) {
            if (a.type != Json::Object) continue;
            Asset as;
            as.name = a.str("name");
            as.url = a.str("browser_download_url");
            if (const Json* sz = a.get("size"); sz && sz->type == Json::Number && sz->n >= 0)
                as.size = std::uint64_t(sz->n);
            std::string digest = a.str("digest");
            if (digest.rfind("sha256:", 0) == 0 && is_hex64(digest.substr(7))) as.sha256 = lower(digest.substr(7));
            if (!as.name.empty() && !as.url.empty()) r.assets.push_back(std::move(as));
        }
    }
    return r;
}

Platform current_platform() {
#if defined(__APPLE__)
    return Platform::MacOS;
#elif defined(_WIN32)
    return Platform::Windows;
#elif defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
    return Platform::LinuxX64;
#else
    return Platform::Other;
#endif
}

namespace {
const char* asset_suffix(Platform p) {
    switch (p) {
        case Platform::MacOS: return "-macOS.dmg";
        case Platform::Windows: return "-Windows-x64.exe";
        case Platform::LinuxX64: return "-Linux-x64.AppImage";
        default: return nullptr;
    }
}
}  // namespace

std::string asset_name(Platform p, const std::string& version) {
    const char* suffix = asset_suffix(p);
    return suffix ? "SuperSaga-" + version + suffix : std::string();
}

const Asset* pick_asset(const Release& r, Platform p) {
    const char* suffix = asset_suffix(p);
    if (!suffix) return nullptr;
    // Prefer the exact expected name, then any SuperSaga-*<suffix>.
    if (auto v = parse_version(r.tag))
        if (const Asset* a = r.find(asset_name(p, to_string(*v)))) return a;
    for (const Asset& a : r.assets)
        if (a.name.rfind("SuperSaga-", 0) == 0 && ends_with(a.name, suffix)) return &a;
    return nullptr;
}

// ---- checksums ------------------------------------------------------------------------------

std::string checksum_for(const std::string& sums_text, const std::string& file) {
    size_t p = 0;
    while (p < sums_text.size()) {
        size_t e = sums_text.find('\n', p);
        std::string line = sums_text.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? sums_text.size() : e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.size() < 66) continue;
        std::string digest = line.substr(0, 64);
        if (!is_hex64(digest) || (line[64] != ' ' && line[64] != '\t')) continue;
        size_t n = 65;
        while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) ++n;
        if (n < line.size() && line[n] == '*') ++n;  // binary-mode marker
        std::string name = line.substr(n);
        if (name.rfind("./", 0) == 0) name = name.substr(2);
        if (name == file) return lower(digest);
    }
    return {};
}

// ---- zip ------------------------------------------------------------------------------------

bool zip_supported() {
#if SUPERSAGA_HAVE_ZLIB
    return true;
#else
    return false;
#endif
}

namespace {
std::uint32_t rd16(const std::vector<std::uint8_t>& b, size_t o) { return std::uint32_t(b[o]) | std::uint32_t(b[o + 1]) << 8; }
std::uint32_t rd32(const std::vector<std::uint8_t>& b, size_t o) { return rd16(b, o) | rd16(b, o + 2) << 16; }

std::uint32_t crc32_of(const std::uint8_t* p, size_t n) {
#if SUPERSAGA_HAVE_ZLIB
    uLong c = crc32(0L, Z_NULL, 0);
    while (n) {
        uInt chunk = uInt(std::min<size_t>(n, 1u << 30));
        c = crc32(c, p, chunk);
        p += chunk;
        n -= chunk;
    }
    return std::uint32_t(c);
#else
    std::uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
#endif
}

// A zip entry name as a safe relative path, or empty if it must be rejected.
fs::path safe_relative(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return {};
    fs::path out;
    size_t p = 0;
    while (p <= name.size()) {
        size_t e = name.find('/', p);
        std::string part = name.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (part == "..") return {};
        if (!part.empty() && part != ".") out /= fs::path(std::u8string(part.begin(), part.end()));
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return out;
}
}  // namespace

bool extract_zip(const fs::path& zip, const fs::path& dest, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::vector<std::uint8_t> b;
    {
        std::ifstream f(zip, std::ios::binary);
        if (!f) return fail(N_("Couldn't open the downloaded update."));
        b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (b.size() < 22) return fail(N_("The downloaded update is not a valid zip file."));
    size_t eocd = std::string::npos;
    for (size_t i = b.size() - 22 + 1; i-- > 0 && b.size() - i <= 22 + 65535;)
        if (rd32(b, i) == 0x06054b50u) { eocd = i; break; }
    if (eocd == std::string::npos) return fail(N_("The downloaded update is not a valid zip file."));
    size_t count = rd16(b, eocd + 10);
    size_t cd = rd32(b, eocd + 16);
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) return fail("Couldn't create a folder for the update: " + ec.message());
    for (size_t k = 0; k < count; ++k) {
        if (cd + 46 > b.size() || rd32(b, cd) != 0x02014b50u) return fail(N_("The update's zip file is damaged."));
        std::uint32_t method = rd16(b, cd + 10), crc = rd32(b, cd + 16);
        size_t csize = rd32(b, cd + 20), usize = rd32(b, cd + 24);
        size_t nlen = rd16(b, cd + 28), xlen = rd16(b, cd + 30), clen = rd16(b, cd + 32);
        size_t local = rd32(b, cd + 42);
        if (cd + 46 + nlen > b.size()) return fail(N_("The update's zip file is damaged."));
        std::string name(reinterpret_cast<const char*>(&b[cd + 46]), nlen);
        cd += 46 + nlen + xlen + clen;
        if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || local == 0xFFFFFFFFu)
            return fail(N_("The update's zip file uses a format SuperSaga doesn't support (zip64)."));
        bool is_dir = !name.empty() && (name.back() == '/' || name.back() == '\\');
        fs::path rel = safe_relative(name);
        if (rel.empty()) {
            if (is_dir) continue;
            return fail("The update contains an unsafe file name: " + name);
        }
        fs::path out = dest / rel;
        if (is_dir) {
            fs::create_directories(out, ec);
            continue;
        }
        if (local + 30 > b.size() || rd32(b, local) != 0x04034b50u) return fail(N_("The update's zip file is damaged."));
        size_t data = local + 30 + rd16(b, local + 26) + rd16(b, local + 28);
        if (data + csize > b.size()) return fail(N_("The update's zip file is cut short."));
        std::vector<std::uint8_t> bytes;
        if (method == 0) {
            if (csize != usize) return fail(N_("The update's zip file is damaged."));
            bytes.assign(b.begin() + std::ptrdiff_t(data), b.begin() + std::ptrdiff_t(data + csize));
        } else if (method == 8) {
#if SUPERSAGA_HAVE_ZLIB
            bytes.resize(usize);
            z_stream z{};
            if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return fail(N_("Couldn't unpack the update."));
            z.next_in = const_cast<Bytef*>(&b[data]);
            z.avail_in = uInt(csize);
            z.next_out = bytes.empty() ? nullptr : bytes.data();
            z.avail_out = uInt(usize);
            int rc = inflate(&z, Z_FINISH);
            size_t produced = usize - z.avail_out;
            inflateEnd(&z);
            if (rc != Z_STREAM_END || produced != usize) return fail("Couldn't unpack " + name + " from the update.");
#else
            return fail(N_("This build of SuperSaga can't unpack zip updates."));
#endif
        } else {
            return fail(N_("The update's zip file uses an unsupported compression method."));
        }
        if (crc32_of(bytes.data(), bytes.size()) != crc) return fail("The update's zip file is damaged (" + name + ").");
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!f) return fail("Couldn't write " + name + ".");
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) return fail("Couldn't write " + name + ".");
    }
    return true;
}

// ---- release notes --------------------------------------------------------------------------

namespace {
std::string clean_inline(std::string s) {
    // Drop HTML comments.
    for (size_t a; (a = s.find("<!--")) != std::string::npos;) {
        size_t e = s.find("-->", a);
        s.erase(a, e == std::string::npos ? std::string::npos : e + 3 - a);
    }
    // [text](url) -> text
    for (size_t a = 0; (a = s.find('[', a)) != std::string::npos;) {
        size_t m = s.find("](", a);
        size_t e = m == std::string::npos ? m : s.find(')', m);
        if (m == std::string::npos || e == std::string::npos || s.find('\n', a) < e) { ++a; continue; }
        s = s.substr(0, a) + s.substr(a + 1, m - a - 1) + s.substr(e + 1);
    }
    // Emphasis and code markers.
    for (const char* mark : {"**", "__", "`"})
        for (size_t a; (a = s.find(mark)) != std::string::npos;) s.erase(a, std::strlen(mark));
    // Pull request links: ".../pull/12" -> "#12".
    for (size_t a = 0; (a = s.find("https://github.com/", a)) != std::string::npos;) {
        size_t e = a;
        while (e < s.size() && !std::isspace(static_cast<unsigned char>(s[e])) && s[e] != ')') ++e;
        std::string url = s.substr(a, e - a);
        size_t pull = url.rfind("/pull/");
        if (pull != std::string::npos && url.find_first_not_of("0123456789", pull + 6) == std::string::npos &&
            pull + 6 < url.size()) {
            s.replace(a, e - a, "#" + url.substr(pull + 6));
        }
        ++a;
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    return s.substr(b);
}
}  // namespace

std::vector<NoteLine> format_notes(const std::string& markdown) {
    std::vector<NoteLine> out;
    std::string text = markdown;
    // Remove multi-line HTML comments up front.
    for (size_t a; (a = text.find("<!--")) != std::string::npos;) {
        size_t e = text.find("-->", a);
        text.erase(a, e == std::string::npos ? std::string::npos : e + 3 - a);
    }
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        std::string line = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? text.size() + 1 : e + 1;
        std::string t = clean_inline(line);
        NoteLine nl;
        if (t.empty()) {
            if (!out.empty() && out.back().kind != NoteLine::Blank) out.push_back({NoteLine::Blank, {}});
            continue;
        }
        if (t[0] == '#') {
            size_t h = t.find_first_not_of('#');
            nl = {NoteLine::Heading, h == std::string::npos ? std::string() : clean_inline(t.substr(h))};
        } else if ((t[0] == '*' || t[0] == '-' || t[0] == '+') && t.size() > 1 && t[1] == ' ') {
            nl = {NoteLine::Bullet, clean_inline(t.substr(2))};
        } else if (t == "---" || t == "***") {
            continue;
        } else {
            nl = {NoteLine::Text, t};
        }
        if (!nl.text.empty()) out.push_back(std::move(nl));
    }
    while (!out.empty() && out.back().kind == NoteLine::Blank) out.pop_back();
    return out;
}

}  // namespace supersaga::update
