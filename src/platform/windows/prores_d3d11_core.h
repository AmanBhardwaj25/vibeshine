/**
 * @file src/platform/windows/prores_d3d11_core.h
 * @brief ProRes encoding of Direct3D 11 frames with D3D11 compute shaders only.
 *
 * Frame path (docs/prores-protocol.md, plan section 2.4):
 *
 *   captured frame (D3D11 texture, keyed mutex)
 *     -> private D3D11 device on the capture adapter
 *     -> prores_dct_cs: RGB -> 10-bit Y'CbCr, 8x8 DCT, coefficients in slice scan order
 *     -> prores_count_cs: exact bytes per (candidate q, slice, plane)
 *     -> prores_choose_cs: finest q within the byte budget, slice offsets, headers
 *     -> prores_write_cs: bit-exact slice data
 *     -> one blocking readback
 *
 * This file has no Sunshine dependencies so it can be driven by a standalone test
 * harness. It only borrows the colour types of the PyroWave D3D11 core.
 */
#pragma once

#include "src/platform/windows/pyrowave_d3d11_core.h"
#include "src/prores/prores_bitstream.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi.h>

namespace prores::d3d11 {
  using pyrowave::d3d11::log_fn;

  struct core_config_t {
    int width = 0;
    int height = 0;
    bool yuv444 = false;
    pyrowave::d3d11::color_matrix_t color_matrix {};  ///< video::color_vectors_from_colorspace(colorspace, true).
    prores::color_t color;
    std::wstring shader_dir;  ///< Directory holding the prores_*.hlsl files.
    float sdr_white_nits = 100.0f;  ///< For transfer_e::sdr_to_pq.
    std::chrono::milliseconds capture_wait {};  ///< Negotiated frame interval, rounded up.
    std::size_t capacity_frame_bytes = 8u << 20;  ///< Largest frame the output buffer holds (the transport limit).
    std::array<int, 16> ladder = prores::default_ladder;
    prores::quant_t quant;
  };

  struct frame_stats_t {
    double gpu_wait_ms = 0;
    std::size_t frame_bytes = 0;
    int q = 0;
  };

  enum class result_e {
    ok,
    skipped,  ///< The frame was dropped (e.g. capture mutex timeout); keep going.
    failed,  ///< The session cannot continue.
  };

  class core_t {
  public:
    ~core_t();

    /**
     * @brief Create the device, buffers and shaders on `adapter`.
     * @return nullptr on failure, after logging why.
     */
    static std::unique_ptr<core_t> create(IDXGIAdapter *adapter, const core_config_t &config, log_fn log);

    /// The private D3D11 device captured textures must be opened on.
    [[nodiscard]] ID3D11Device *device() const;

    /// Slice and coefficient layout of this session.
    [[nodiscard]] const prores::layout_t &layout() const;

    /**
     * @brief Convert and encode one frame into `out` (frame atom, frame header, picture).
     *
     * `max_frame_bytes` is the byte budget of the whole frame. `critical_bytes` receives the
     * frame atom, frame header, picture header and slice table. `skipped` also covers a frame
     * that does not fit `capacity_frame_bytes` at the coarsest q.
     */
    result_e encode(
      const pyrowave::d3d11::source_t &source,
      std::size_t max_frame_bytes,
      std::vector<std::uint8_t> &out,
      std::size_t &critical_bytes,
      frame_stats_t *stats = nullptr
    );

    /// Test hook: wait for the GPU and copy the coefficient buffer back (layout().total_coeffs values).
    bool read_coefficients(std::vector<std::int16_t> &coeffs);

    /// Test hook: bytes per (candidate, slice, plane) of the last frame, index (c * slices + slice) * 3 + plane.
    bool read_counts(std::vector<std::uint32_t> &counts);

  private:
    friend bool probe(const LUID *, const std::wstring &, std::string &);
    core_t();
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };

  /**
   * @brief Check that a device can be created on an adapter and every shader compiles.
   * @param luid Adapter LUID, or nullptr for the default adapter.
   * @param shader_dir Directory holding the prores_*.hlsl files.
   * @param detail Receives a human-readable reason on failure.
   */
  bool probe(const LUID *luid, const std::wstring &shader_dir, std::string &detail);
}  // namespace prores::d3d11
