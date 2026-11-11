#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace supersaga {

using Sha1Digest = std::array<std::uint8_t, 20>;

Sha1Digest sha1(const std::uint8_t* data, std::size_t len);
std::string to_hex(const Sha1Digest& d);

}  // namespace supersaga
