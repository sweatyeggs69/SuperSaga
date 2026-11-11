#include "app/credentials.h"

#include <fstream>
#include <iterator>
#include <system_error>

#if defined(__APPLE__)
#include <Security/Security.h>
#elif defined(_WIN32)
#include <windows.h>
#include <wincred.h>
#endif

namespace supersaga::credentials {
namespace fs = std::filesystem;

#if defined(__APPLE__)
namespace {
constexpr const char* kService = "SuperSaga RetroAchievements";

struct CF {
    CFTypeRef ref = nullptr;
    explicit CF(CFTypeRef r) : ref(r) {}
    ~CF() { if (ref) CFRelease(ref); }
    CF(const CF&) = delete;
    CF& operator=(const CF&) = delete;
};

CFStringRef cfstr(const std::string& s) {
    return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()), CFIndex(s.size()),
                                   kCFStringEncodingUTF8, false);
}

CFMutableDictionaryRef query_for(const std::string& user, CF& service, CF& account) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    service.ref = cfstr(kService);
    account.ref = cfstr(user);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service.ref);
    CFDictionarySetValue(q, kSecAttrAccount, account.ref);
    return q;
}
}  // namespace

bool store(const fs::path&, const std::string& user, const std::string& secret) {
    CF service(nullptr), account(nullptr);
    CF query(query_for(user, service, account));
    SecItemDelete(static_cast<CFDictionaryRef>(query.ref));
    CF data(CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(secret.data()), CFIndex(secret.size())));
    CFDictionarySetValue((CFMutableDictionaryRef)query.ref, kSecValueData, data.ref);
    return SecItemAdd(static_cast<CFDictionaryRef>(query.ref), nullptr) == errSecSuccess;
}

std::optional<std::string> load(const fs::path&, const std::string& user) {
    CF service(nullptr), account(nullptr);
    CF query(query_for(user, service, account));
    CFDictionarySetValue((CFMutableDictionaryRef)query.ref, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue((CFMutableDictionaryRef)query.ref, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef out = nullptr;
    if (SecItemCopyMatching(static_cast<CFDictionaryRef>(query.ref), &out) != errSecSuccess || !out)
        return std::nullopt;
    CF data(out);
    auto d = static_cast<CFDataRef>(out);
    return std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(d)), size_t(CFDataGetLength(d)));
}

void erase(const fs::path&, const std::string& user) {
    CF service(nullptr), account(nullptr);
    CF query(query_for(user, service, account));
    SecItemDelete(static_cast<CFDictionaryRef>(query.ref));
}

const char* location() { return "your macOS Keychain"; }

#elif defined(_WIN32)
namespace {
std::wstring target(const std::string& user) {
    std::string t = "SuperSaga/RetroAchievements/" + user;
    int n = MultiByteToWideChar(CP_UTF8, 0, t.data(), int(t.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, t.data(), int(t.size()), w.data(), n);
    return w;
}
}  // namespace

bool store(const fs::path&, const std::string& user, const std::string& secret) {
    std::wstring name = target(user);
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = name.data();
    c.CredentialBlobSize = DWORD(secret.size());
    c.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&c, 0) != FALSE;
}

std::optional<std::string> load(const fs::path&, const std::string& user) {
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target(user).c_str(), CRED_TYPE_GENERIC, 0, &c)) return std::nullopt;
    std::string s(reinterpret_cast<const char*>(c->CredentialBlob), c->CredentialBlobSize);
    CredFree(c);
    return s;
}

void erase(const fs::path&, const std::string& user) { CredDeleteW(target(user).c_str(), CRED_TYPE_GENERIC, 0); }

const char* location() { return "Windows Credential Manager"; }

#else
namespace {
fs::path token_file(const fs::path& pref_dir) { return pref_dir / "retroachievements.token"; }
}  // namespace

bool store(const fs::path& pref_dir, const std::string& user, const std::string& secret) {
    fs::path p = token_file(pref_dir);
    std::error_code ec;
    fs::remove(p, ec);
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) return false;
    }
    // Restrict before writing the secret.
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    if (ec) return false;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << user << '\n' << secret << '\n';
    return bool(f);
}

std::optional<std::string> load(const fs::path& pref_dir, const std::string& user) {
    std::ifstream f(token_file(pref_dir), std::ios::binary);
    std::string u, secret;
    if (!std::getline(f, u) || !std::getline(f, secret) || u != user || secret.empty()) return std::nullopt;
    return secret;
}

void erase(const fs::path& pref_dir, const std::string&) {
    std::error_code ec;
    fs::remove(token_file(pref_dir), ec);
}

const char* location() { return "a private file in the settings folder"; }
#endif

}  // namespace supersaga::credentials
