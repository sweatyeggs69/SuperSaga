#pragma once
// SHA-256 (FIPS 180-4), used to verify downloaded updates against the release's SHA256SUMS.txt.
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace supersaga {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t len);
    std::array<std::uint8_t, 32> finish();

private:
    void block(const std::uint8_t* p);
    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buf_{};
    std::size_t buf_len_ = 0;
    std::uint64_t total_ = 0;
};

// Lowercase hex digest of a byte range / a whole file ("" if the file can't be read).
std::string sha256_hex(const void* data, std::size_t len);
std::string sha256_file_hex(const std::filesystem::path& file);

}  // namespace supersaga
