#ifndef DELTAFORMAT_HPP
#define DELTAFORMAT_HPP

#include <cstdint>

// Delta file header: 4-byte magic + 4-byte BE version.
inline constexpr uint8_t DELTA_MAGIC[4] = { 'R', 'H', 'D', 0x00 };
inline constexpr uint32_t DELTA_FORMAT_VERSION = 1;
inline constexpr size_t DELTA_HEADER_SIZE = 8;

#endif
