// Updater logic: versions, release JSON, asset choice, SHA256SUMS.txt, SHA-256 and zip unpacking.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "update/sha256.h"
#include "update/update_core.h"

namespace fs = std::filesystem;
using namespace supersaga;
using namespace supersaga::update;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static int cmp(const char* a, const char* b) { return compare_versions(*parse_version(a), *parse_version(b)); }

static std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

int main() {
    // SHA-256 test vectors (FIPS 180-2).
    CHECK(sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    {
        const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        CHECK(sha256_hex(m, 56) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        std::string million(1000000, 'a');
        CHECK(sha256_hex(million.data(), million.size()) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        Sha256 s;  // incremental, odd chunk sizes
        for (size_t i = 0; i < million.size(); i += 777) s.update(million.data() + i, std::min<size_t>(777, million.size() - i));
        (void)s.finish();
    }

    // Versions.
    CHECK(parse_version("v1.2.3").has_value());
    CHECK(to_string(*parse_version("v1.2.3")) == "1.2.3");
    CHECK(to_string(*parse_version("2")) == "2.0.0");
    CHECK(to_string(*parse_version("1.4")) == "1.4.0");
    CHECK(to_string(*parse_version("1.0.0-rc.1+build5")) == "1.0.0-rc.1");
    CHECK(!parse_version("latest").has_value());
    CHECK(!parse_version("").has_value());
    CHECK(cmp("1.0.0", "0.1.0") > 0);
    CHECK(cmp("v0.1.0", "0.1.0") == 0);
    CHECK(cmp("0.10.0", "0.9.9") > 0);
    CHECK(cmp("1.0.0-rc.1", "1.0.0") < 0);
    CHECK(cmp("1.0.0", "1.0.0-rc.1") > 0);
    CHECK(cmp("1.0.1-beta", "1.0.0") > 0);

    // Release JSON (shape of GitHub's /releases/latest).
    const char* json = R"({
      "url": "https://api.github.com/repos/sweatyeggs69/SuperSaga/releases/1",
      "html_url": "https://github.com/sweatyeggs69/SuperSaga/releases/tag/v1.2.0",
      "tag_name": "v1.2.0", "name": "SuperSaga 1.2.0", "draft": false, "prerelease": false,
      "body": "## What's Changed\r\n* Faster é \"saves\" by @sweatyeggs69 in https://github.com/sweatyeggs69/SuperSaga/pull/12\r\n\r\n**Full Changelog**: https://github.com/sweatyeggs69/SuperSaga/compare/v1.1.0...v1.2.0🌟",
      "assets": [
        {"name": "SuperSaga-1.2.0-macOS.dmg", "size": 4000000, "digest": "sha256:ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
         "browser_download_url": "https://github.com/sweatyeggs69/SuperSaga/releases/download/v1.2.0/SuperSaga-1.2.0-macOS.dmg"},
        {"name": "SuperSaga-1.2.0-Windows-x64.exe", "size": 3000000, "digest": null,
         "browser_download_url": "https://github.com/sweatyeggs69/SuperSaga/releases/download/v1.2.0/SuperSaga-1.2.0-Windows-x64.exe"},
        {"name": "SuperSaga-1.2.0-Linux-x64.AppImage", "size": 5000000, "uploader": {"login": "github-actions[bot]", "id": 41898282},
         "browser_download_url": "https://github.com/sweatyeggs69/SuperSaga/releases/download/v1.2.0/SuperSaga-1.2.0-Linux-x64.AppImage"},
        {"name": "SHA256SUMS.txt", "size": 300,
         "browser_download_url": "https://github.com/sweatyeggs69/SuperSaga/releases/download/v1.2.0/SHA256SUMS.txt"}
      ], "reactions": {"+1": 2}, "mentions_count": 1.5e1
    })";
    std::string err;
    auto rel = parse_release_json(json, &err);
    CHECK(rel.has_value());
    if (rel) {
        CHECK(rel->tag == "v1.2.0");
        CHECK(rel->assets.size() == 4);
        CHECK(rel->notes.find("Faster \xc3\xa9 \"saves\"") != std::string::npos);
        CHECK(rel->notes.find("\xf0\x9f\x8c\x9f") != std::string::npos);  // surrogate pair -> U+1F31F
        const Asset* mac = pick_asset(*rel, Platform::MacOS);
        CHECK(mac && mac->name == "SuperSaga-1.2.0-macOS.dmg" && mac->size == 4000000);
        CHECK(mac && mac->sha256 == "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
        const Asset* win = pick_asset(*rel, Platform::Windows);
        CHECK(win && win->name == "SuperSaga-1.2.0-Windows-x64.exe" && win->sha256.empty());
        const Asset* lin = pick_asset(*rel, Platform::LinuxX64);
        CHECK(lin && lin->name == "SuperSaga-1.2.0-Linux-x64.AppImage");
        CHECK(pick_asset(*rel, Platform::Other) == nullptr);
        CHECK(rel->find("SHA256SUMS.txt") != nullptr);

        auto notes = format_notes(rel->notes);
        CHECK(notes.size() >= 3);
        if (notes.size() >= 3) {
            CHECK(notes[0].kind == NoteLine::Heading && notes[0].text == "What's Changed");
            CHECK(notes[1].kind == NoteLine::Bullet);
            CHECK(notes[1].text.find("in #12") != std::string::npos);
            CHECK(notes.back().text.rfind("Full Changelog:", 0) == 0);
        }
    }
    CHECK(!parse_release_json(R"({"message": "Not Found", "documentation_url": "x"})", &err));
    CHECK(err == "No releases have been published yet.");
    CHECK(!parse_release_json("<html>rate limited</html>", &err));
    CHECK(!parse_release_json(R"({"tag_name": "v1", )", &err));
    CHECK(asset_name(Platform::Windows, "1.0.0") == "SuperSaga-1.0.0-Windows-x64.exe");
    CHECK(asset_name(Platform::Other, "1.0.0").empty());

    // SHA256SUMS.txt (sha256sum output, plus variations).
    std::string sums =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  SuperSaga-1.2.0-macOS.dmg\n"
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD *SuperSaga-1.2.0-Windows-x64.exe\r\n"
        "nothex  SuperSaga-1.2.0-Linux-x64.AppImage\n"
        "248d6a61d20638b8e5c0262693c3d0f4ad1c8b80a4da07ec7e15e0f47b3f5f59  ./SuperSaga-1.2.0-Linux-x64.AppImage";
    CHECK(checksum_for(sums, "SuperSaga-1.2.0-macOS.dmg") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(checksum_for(sums, "SuperSaga-1.2.0-Windows-x64.exe") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(checksum_for(sums, "SuperSaga-1.2.0-Linux-x64.AppImage") == "248d6a61d20638b8e5c0262693c3d0f4ad1c8b80a4da07ec7e15e0f47b3f5f59");
    CHECK(checksum_for(sums, "SuperSaga-1.2.0-macOS.dm").empty());
    CHECK(checksum_for(sums, "missing").empty());

    // Zip unpacking (the Windows release zip is made by `cmake -E tar --format=zip`).
    fs::path data = SUPERSAGA_TEST_DATA;
    fs::path out = fs::temp_directory_path() / "supersaga_update_test";
    std::error_code ec;
    fs::remove_all(out, ec);
    if (zip_supported()) {
        CHECK(extract_zip(data / "update-deflate.zip", out / "a", &err));
        CHECK(read(out / "a" / "SuperSaga" / "SuperSaga.exe") == "SuperSaga test file\n");
        std::string big = read(out / "a" / "SuperSaga" / "licenses" / "big.txt");
        CHECK(big.size() == 55000 && big.rfind("Saga star ", 0) == 0);
    } else {
        std::printf("zlib not available: skipping the deflate zip check\n");
    }
    CHECK(extract_zip(data / "update-stored.zip", out / "b", &err));
    CHECK(read(out / "b" / "SuperSaga" / "SuperSaga.exe") == "stored entry\n");
    CHECK(!extract_zip(data / "update-unsafe.zip", out / "c", &err));
    CHECK(!fs::exists(out / "evil.txt"));
    CHECK(!extract_zip(data / "missing.zip", out / "d", &err));
    fs::remove_all(out, ec);

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("update tests passed\n");
    return 0;
}
