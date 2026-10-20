/**
 * @file src/prores/prores_bitstream.h
 * @brief Dependency-free CPU reference ProRes encoder (source of truth for the GPU encoder).
 *
 * The entropy coder and size estimate follow FFmpeg's libavcodec/proresenc_kostya.c
 * (LGPL 2.1 or later), which is license compatible with this GPL 3 project.
 * Layout and byte format: docs/prores-protocol.md.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace prores {
  struct slice_t {
    std::uint16_t mb_x, mb_y, mbs;
    std::uint32_t coeff_offset[3];
  };

  struct layout_t {
    int width = 0, height = 0, mbs_per_slice = 8;
    bool yuv444 = false;
    int mb_width = 0, mb_height = 0, slices_per_row = 0;
    std::vector<slice_t> slices;
    std::uint32_t total_coeffs = 0;  ///< all three planes; coeff_offset values are global

    static layout_t make(int width, int height, bool yuv444, int mbs_per_slice = 8);
    int blocks_per_mb(int plane) const;  ///< 4 for luma, 2 or 4 for chroma
    std::size_t critical_bytes() const;  ///< 8 + 20 + 8 + 2 * slices
    int plane_width(int plane) const;  ///< sample columns of a plane
  };

  struct color_t {
    std::uint8_t primaries = 1, transfer = 1, matrix = 1;
  };

  /// Flat 4s and send = false by default.
  struct quant_t {
    std::uint8_t luma[64], chroma[64];
    bool send = false;
    quant_t() {
      for (int i = 0; i < 64; i++) {
        luma[i] = chroma[i] = 4;
      }
    }
  };

  /// 10-bit planes (Cb/Cr at chroma resolution) -> coefficients in the docs section 3.5 order.
  /// `coeffs` holds layout.total_coeffs values.
  void forward_dct(const layout_t &, const std::uint16_t *const planes[3], const std::size_t strides[3], std::int16_t *coeffs);

  /// Exact bytes of one plane of one slice at scale q (the write pass emits exactly this many).
  std::size_t plane_bytes(const layout_t &, const std::int16_t *coeffs, std::size_t slice, int plane, int q, const quant_t &);

  /// Builds a whole frame (frame atom, frame header, picture). Returns out.size().
  std::size_t write_frame(const layout_t &, const std::int16_t *coeffs, int q, const color_t &, const quant_t &, std::vector<std::uint8_t> &out);

  /// Exact frame size at q (same value write_frame would produce).
  std::size_t frame_bytes(const layout_t &, const std::int16_t *coeffs, int q, const quant_t &);

  /// Smallest ladder index whose frame fits the budget; the last index when none fits.
  int choose_ladder_index(const layout_t &, const std::int16_t *coeffs, std::size_t budget_frame_bytes, const std::array<int, 16> &ladder, const quant_t &);

  /// Candidate q values (P0 result), lowest q first.
  inline constexpr std::array<int, 16> default_ladder = {4, 5, 6, 8, 10, 12, 16, 20, 25, 31, 39, 49, 62, 78, 98, 128};

  struct parsed_t {
    int width = 0, height = 0;
    bool yuv444 = false;
    std::size_t picture_offset = 0, table_offset = 0, data_offset = 0;
    std::vector<std::uint32_t> slice_offsets, slice_sizes;  ///< offsets from the start of the frame
  };

  bool parse_frame(std::span<const std::uint8_t>, parsed_t &);
}  // namespace prores
