/**
 * @file src/prores/prores_protocol.h
 * @brief Wire constants of the ChloroFrame ProRes extension (see docs/prores-protocol.md).
 */
#pragma once

#include <cstdint>

namespace prores::protocol {
  inline constexpr int BITSTREAM_FORMAT = 4;
  inline constexpr std::uint32_t SCM_PRORES_422 = 0x08000000;
  inline constexpr std::uint32_t SCM_PRORES_444 = 0x10000000;
  inline constexpr int DESCRIBE_VERSION = 1;  ///< a=x-cf-prores:1
  inline constexpr int FRAME_PREFIX_BYTES = 8 + 20;  ///< frame atom + frame header
}  // namespace prores::protocol
