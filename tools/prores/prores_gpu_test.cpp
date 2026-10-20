/**
 * @file tools/prores/prores_gpu_test.cpp
 * @brief Windows: compares the GPU core's coefficients with the CPU reference (WARP or a real GPU).
 *
 * usage: prores_gpu_test <shader dir>
 */
#include "src/platform/windows/prores_d3d11_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {
  int failures = 0;

  #define CHECK(cond) \
    do { \
      if (!(cond)) { \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
      } \
    } while (0)

  // BT.709 limited range, 10-bit code values normalized by 1023 (video::color_vectors_from_colorspace shape).
  pyrowave::d3d11::color_matrix_t bt709_limited() {
    pyrowave::d3d11::color_matrix_t m {};
    const float kr = 0.2126f, kb = 0.0722f, kg = 1.0f - kr - kb;
    const float v_div = 2.0f * (1.0f - kr), u_div = 2.0f * (1.0f - kb);
    const float y[4] = {kr, kg, kb, 0.0f};
    const float u[4] = {-kr / u_div, -kg / u_div, 0.5f, 0.5f};
    const float v[4] = {0.5f, -kg / v_div, -kb / v_div, 0.5f};
    std::copy(y, y + 4, m.color_vec_y);
    std::copy(u, u + 4, m.color_vec_u);
    std::copy(v, v + 4, m.color_vec_v);
    m.range_y[0] = 876.0f / 1023.0f;
    m.range_y[1] = 64.0f / 1023.0f;
    m.range_uv[0] = 896.0f / 1023.0f;
    m.range_uv[1] = 64.0f / 1023.0f;
    return m;
  }

  // Deterministic synthetic BGRA image: gradients, hard edges, thin lines, fine texture.
  void synth_pixel(int x, int y, int w, int h, unsigned char bgra[4]) {
    int r = x * 255 / std::max(1, w - 1);
    int g = y * 255 / std::max(1, h - 1);
    int b = ((x ^ y) & 0xFF);
    if (((x / 37) + (y / 29)) & 1) {
      r = 255 - r;
      b = 255 - b;
    }
    if (x % 7 == 0 || y % 11 == 0) {
      r = g = b = 255;
    }
    if (x > w / 2 && y > h / 2 && ((x * 31 + y * 17) % 5) == 0) {
      r = g = 0;
      b = 40;
    }
    bgra[0] = (unsigned char) b;
    bgra[1] = (unsigned char) g;
    bgra[2] = (unsigned char) r;
    bgra[3] = 255;
  }

  // CPU reference of the shader's conversion, in the same float math (no rounding until the end).
  void convert_pixel(const pyrowave::d3d11::color_matrix_t &m, const unsigned char bgra[4], float &y, float &u, float &v) {
    const float r = bgra[2] / 255.0f, g = bgra[1] / 255.0f, b = bgra[0] / 255.0f;
    auto dot = [&](const float *c) { return c[0] * r + c[1] * g + c[2] * b + c[3]; };
    y = (dot(m.color_vec_y) * m.range_y[0] + m.range_y[1]) * 1023.0f;
    u = (dot(m.color_vec_u) * m.range_uv[0] + m.range_uv[1]) * 1023.0f;
    v = (dot(m.color_vec_v) * m.range_uv[0] + m.range_uv[1]) * 1023.0f;
  }

  std::uint16_t code10(float v) {
    return (std::uint16_t) std::clamp(std::lround(v), 0L, 1023L);
  }

  void log_to_stdout(int level, const std::string &message) {
    std::printf("[%d] %s\n", level, message.c_str());
  }

  bool run_case(IDXGIAdapter *adapter, const std::wstring &shader_dir, int w, int h, bool yuv444) {
    std::printf("case %dx%d %s\n", w, h, yuv444 ? "4:4:4" : "4:2:2");
    prores::d3d11::core_config_t config;
    config.width = w;
    config.height = h;
    config.yuv444 = yuv444;
    config.color_matrix = bt709_limited();
    config.shader_dir = shader_dir;
    config.capture_wait = std::chrono::milliseconds(100);
    auto core = prores::d3d11::core_t::create(adapter, config, log_to_stdout);
    if (!core) {
      std::printf("FAIL core creation\n");
      failures++;
      return false;
    }

    std::vector<unsigned char> bgra(std::size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        synth_pixel(x, y, w, h, &bgra[(std::size_t(y) * w + x) * 4]);
      }
    }
    D3D11_TEXTURE2D_DESC td {};
    td.Width = UINT(w);
    td.Height = UINT(h);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init {bgra.data(), UINT(w * 4), 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(core->device()->CreateTexture2D(&td, &init, &texture)) || FAILED(core->device()->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
      std::printf("FAIL source texture\n");
      failures++;
      return false;
    }

    pyrowave::d3d11::source_t source;
    source.srv = srv.Get();
    source.width = UINT(w);
    source.height = UINT(h);
    std::vector<std::uint8_t> out;
    std::size_t critical = 0;
    CHECK(core->encode(source, 1 << 20, out, critical) == prores::d3d11::result_e::skipped);
    std::vector<std::int16_t> gpu;
    CHECK(core->read_coefficients(gpu));

    // CPU reference planes at plane resolution.
    const auto &layout = core->layout();
    const int cw = yuv444 ? w : (w + 1) / 2;
    std::vector<std::uint16_t> py(std::size_t(w) * h), pcb(std::size_t(cw) * h), pcr(std::size_t(cw) * h);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        float fy, fu, fv;
        convert_pixel(config.color_matrix, &bgra[(std::size_t(y) * w + x) * 4], fy, fu, fv);
        py[std::size_t(y) * w + x] = code10(fy);
        if (yuv444) {
          pcb[std::size_t(y) * cw + x] = code10(fu);
          pcr[std::size_t(y) * cw + x] = code10(fv);
        }
      }
      for (int cx = 0; !yuv444 && cx < cw; cx++) {
        float u[2], v[2], fy;
        for (int k = 0; k < 2; k++) {
          const int x = std::min(cx * 2 + k, w - 1);
          convert_pixel(config.color_matrix, &bgra[(std::size_t(y) * w + x) * 4], fy, u[k], v[k]);
        }
        pcb[std::size_t(y) * cw + cx] = code10((u[0] + u[1]) * 0.5f);
        pcr[std::size_t(y) * cw + cx] = code10((v[0] + v[1]) * 0.5f);
      }
    }
    const std::uint16_t *planes[3] = {py.data(), pcb.data(), pcr.data()};
    const std::size_t strides[3] = {std::size_t(w), std::size_t(cw), std::size_t(cw)};
    std::vector<std::int16_t> cpu(layout.total_coeffs);
    prores::forward_dct(layout, planes, strides, cpu.data());

    CHECK(gpu.size() == cpu.size());
    std::size_t exact = 0, off_by_one = 0, bad = 0, first_bad = 0;
    int max_diff = 0;
    for (std::size_t i = 0; i < std::min(gpu.size(), cpu.size()); i++) {
      const int d = std::abs(int(gpu[i]) - int(cpu[i]));
      max_diff = std::max(max_diff, d);
      if (d == 0) {
        exact++;
      } else if (d <= 2) {
        off_by_one++;
      } else {
        if (!bad) {
          first_bad = i;
        }
        bad++;
      }
    }
    const double exact_pct = 100.0 * double(exact) / double(cpu.size());
    std::printf("  coefficients %zu: exact %.3f%%, within 2 %zu, beyond 2 %zu (max diff %d)\n", cpu.size(), exact_pct, off_by_one, bad, max_diff);
    if (bad) {
      std::printf("  first bad index %zu: gpu %d cpu %d\n", first_bad, gpu[first_bad], cpu[first_bad]);
    }
    CHECK(bad == 0);
    CHECK(exact_pct >= 99.9);
    return true;
  }
}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::printf("usage: prores_gpu_test <shader dir>\n");
    return 2;
  }
  const std::string dir = argv[1];
  const std::wstring shader_dir(dir.begin(), dir.end());

  ComPtr<IDXGIFactory4> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    std::printf("FAIL CreateDXGIFactory1\n");
    return 1;
  }
  ComPtr<IDXGIAdapter> adapter;
  for (UINT i = 0; SUCCEEDED(factory->EnumAdapters(i, &adapter)); i++) {
    DXGI_ADAPTER_DESC desc {};
    adapter->GetDesc(&desc);
    if (desc.VendorId != 0x1414) {  // skip "Microsoft Basic Render Driver" unless nothing else exists
      break;
    }
    adapter.Reset();
  }
  if (!adapter) {
    ComPtr<IDXGIAdapter1> warp;
    if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
      warp.As(&adapter);
    }
  }
  if (!adapter) {
    std::printf("FAIL no adapter\n");
    return 1;
  }
  DXGI_ADAPTER_DESC desc {};
  adapter->GetDesc(&desc);
  std::printf("adapter: %ls\n", desc.Description);

  std::string detail;
  const bool probed = prores::d3d11::probe(&desc.AdapterLuid, shader_dir, detail);
  std::printf("probe: %s %s\n", probed ? "ok" : "FAILED", detail.c_str());
  CHECK(probed);

  const int sizes[][2] = {{64, 64}, {17, 9}, {1920, 1080}, {3024, 1890}};
  for (const auto &s : sizes) {
    for (bool yuv444 : {false, true}) {
      run_case(adapter.Get(), shader_dir, s[0], s[1], yuv444);
    }
  }
  std::printf(failures ? "FAILED (%d)\n" : "PASS\n", failures);
  return failures ? 1 : 0;
}
