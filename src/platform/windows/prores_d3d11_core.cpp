/**
 * @file src/platform/windows/prores_d3d11_core.cpp
 * @brief ProRes encoding of Direct3D 11 frames with D3D11 compute shaders only.
 *
 * Device creation, shader compilation and the capture-mutex handling follow
 * pyrowave_d3d11_core.cpp. The bitstream layout and the transform follow
 * FFmpeg's ProRes encoders (LGPL 2.1 or later, license compatible with GPL 3).
 */
#include "prores_d3d11_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace prores::d3d11 {
  namespace {
    using pyrowave::d3d11::transfer_e;

    // Matches params_cbuffer (b2) in prores_dct_cs.hlsl.
    struct dct_params_t {
      std::uint32_t luma_size[2];
      float content_offset[2];
      float content_scale[2];
      std::uint32_t blank;
      std::int32_t rotate_texture_steps;
      std::uint32_t mb_width;
      std::uint32_t padding;
    };

    static_assert(sizeof(dct_params_t) % 16 == 0, "Constant buffers are sized in 16-byte units");

    // Matches sdr_to_pq_params_cbuffer (b1) in include/convert_sdr_to_pq_base.hlsl.
    struct sdr_to_pq_params_t {
      float sdr_white_nits;
      float padding[3];
    };

    std::string hresult_string(HRESULT hr) {
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
      return buffer;
    }
  }  // namespace

  struct core_t::impl_t {
    core_config_t config;
    log_fn log;
    prores::layout_t layout;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    ComPtr<ID3D11Buffer> coeffs;
    ComPtr<ID3D11UnorderedAccessView> coeffs_uav;
    ComPtr<ID3D11Buffer> mb_info;
    ComPtr<ID3D11ShaderResourceView> mb_info_srv;
    ComPtr<ID3D11Buffer> color_matrix;
    ComPtr<ID3D11Buffer> sdr_to_pq_params;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11SamplerState> sampler;
    std::map<transfer_e, ComPtr<ID3D11ComputeShader>> dct_shaders;
    dct_params_t last_params {};
    bool params_valid = false;

    void message(int level, const std::string &text) const {
      if (log) {
        log(level, "ProRes: " + text);
      }
    }

    void error(const std::string &text) const {
      message(3, text);
    }

    void warning(const std::string &text) const {
      message(2, text);
    }

    bool create_device(IDXGIAdapter *adapter) {
      const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
      const D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
      HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, 0, levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &device, nullptr, &context);
      if (FAILED(hr)) {
        error("D3D11CreateDevice failed [" + hresult_string(hr) + "]");
        return false;
      }

      ComPtr<IDXGIDevice> dxgi_device;
      if (SUCCEEDED(device.As(&dxgi_device))) {
        // Same priority as Sunshine's other encoder devices; needs elevation to take effect.
        (void) dxgi_device->SetGPUThreadPriority(7);
      }
      return true;
    }

    bool create_buffers() {
      const UINT coeff_bytes = (layout.total_coeffs * 2 + 3) & ~3u;
      D3D11_BUFFER_DESC bd {};
      bd.ByteWidth = coeff_bytes;
      bd.Usage = D3D11_USAGE_DEFAULT;
      bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
      bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
      HRESULT hr = device->CreateBuffer(&bd, nullptr, &coeffs);
      if (FAILED(hr)) {
        error("coefficient buffer creation failed [" + hresult_string(hr) + "]");
        return false;
      }
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud {};
      ud.Format = DXGI_FORMAT_R32_TYPELESS;
      ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      ud.Buffer.NumElements = coeff_bytes / 4;
      ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
      hr = device->CreateUnorderedAccessView(coeffs.Get(), &ud, &coeffs_uav);
      if (FAILED(hr)) {
        error("coefficient view creation failed [" + hresult_string(hr) + "]");
        return false;
      }

      // Per macroblock: coefficient offsets of the three planes, then mb_in_slice | mbs << 16.
      std::vector<std::uint32_t> info(std::size_t(layout.mb_width) * layout.mb_height * 4);
      for (const auto &slice : layout.slices) {
        for (int i = 0; i < slice.mbs; i++) {
          auto *entry = &info[(std::size_t(slice.mb_y) * layout.mb_width + slice.mb_x + i) * 4];
          for (int p = 0; p < 3; p++) {
            entry[p] = slice.coeff_offset[p];
          }
          entry[3] = std::uint32_t(i) | (std::uint32_t(slice.mbs) << 16);
        }
      }
      bd = {};
      bd.ByteWidth = UINT(info.size() * sizeof(std::uint32_t));
      bd.Usage = D3D11_USAGE_IMMUTABLE;
      bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      bd.StructureByteStride = 16;
      D3D11_SUBRESOURCE_DATA init {info.data(), 0, 0};
      hr = device->CreateBuffer(&bd, &init, &mb_info);
      if (FAILED(hr)) {
        error("macroblock table creation failed [" + hresult_string(hr) + "]");
        return false;
      }
      D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
      sd.Format = DXGI_FORMAT_UNKNOWN;
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
      sd.Buffer.NumElements = UINT(info.size() / 4);
      hr = device->CreateShaderResourceView(mb_info.Get(), &sd, &mb_info_srv);
      if (FAILED(hr)) {
        error("macroblock table view creation failed [" + hresult_string(hr) + "]");
        return false;
      }
      return true;
    }

    bool create_constants() {
      D3D11_BUFFER_DESC cb {};
      cb.ByteWidth = sizeof(config.color_matrix);
      cb.Usage = D3D11_USAGE_IMMUTABLE;
      cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      D3D11_SUBRESOURCE_DATA init {&config.color_matrix, 0, 0};
      HRESULT hr = device->CreateBuffer(&cb, &init, &color_matrix);
      if (FAILED(hr)) {
        error("colour matrix buffer creation failed [" + hresult_string(hr) + "]");
        return false;
      }

      sdr_to_pq_params_t sdr {config.sdr_white_nits, {}};
      cb.ByteWidth = sizeof(sdr);
      init = {&sdr, 0, 0};
      hr = device->CreateBuffer(&cb, &init, &sdr_to_pq_params);
      if (FAILED(hr)) {
        error("SDR-to-PQ buffer creation failed [" + hresult_string(hr) + "]");
        return false;
      }

      cb.ByteWidth = sizeof(dct_params_t);
      cb.Usage = D3D11_USAGE_DEFAULT;
      hr = device->CreateBuffer(&cb, nullptr, &params);
      if (FAILED(hr)) {
        error("parameter buffer creation failed [" + hresult_string(hr) + "]");
        return false;
      }

      D3D11_SAMPLER_DESC sd {};
      sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
      sd.MaxLOD = D3D11_FLOAT32_MAX;
      hr = device->CreateSamplerState(&sd, &sampler);
      if (FAILED(hr)) {
        error("sampler creation failed [" + hresult_string(hr) + "]");
        return false;
      }
      return true;
    }

    ID3D11ComputeShader *dct_shader_for(transfer_e transfer) {
      auto &shader = dct_shaders[transfer];
      if (shader) {
        return shader.Get();
      }

      std::vector<D3D_SHADER_MACRO> macros;
      switch (transfer) {
        case transfer_e::pq:
          macros.push_back({"PQ", "1"});
          break;
        case transfer_e::linear_sdr:
          macros.push_back({"LINEAR", "1"});
          break;
        case transfer_e::sdr_to_pq:
          macros.push_back({"SDR_TO_PQ", "1"});
          break;
        case transfer_e::unorm:
          break;
      }
      if (config.yuv444) {
        macros.push_back({"YUV444", "1"});
      }
      macros.push_back({nullptr, nullptr});

      const std::wstring path = config.shader_dir + L"/prores_dct_cs.hlsl";
      ComPtr<ID3DBlob> code;
      ComPtr<ID3DBlob> messages;
      HRESULT hr = D3DCompileFromFile(path.c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, "main_cs", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
      if (FAILED(hr)) {
        std::string detail = messages ? std::string(static_cast<const char *>(messages->GetBufferPointer()), messages->GetBufferSize()) : std::string("no compiler output");
        error("compiling prores_dct_cs.hlsl failed [" + hresult_string(hr) + "]: " + detail);
        return nullptr;
      }
      hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
      if (FAILED(hr)) {
        error("CreateComputeShader failed [" + hresult_string(hr) + "]");
        shader.Reset();
        return nullptr;
      }
      return shader.Get();
    }

    void update_params(const pyrowave::d3d11::source_t *source) {
      dct_params_t p {};
      p.luma_size[0] = std::uint32_t(config.width);
      p.luma_size[1] = std::uint32_t(config.height);
      p.mb_width = std::uint32_t(layout.mb_width);
      p.blank = source ? 0 : 1;
      if (source && source->width && source->height) {
        p.rotate_texture_steps = source->rotate_texture_steps;
        // Letterbox the picture into the stream extent, preserving its aspect ratio.
        // A quarter turn swaps the displayed width and height.
        const bool swap = (source->rotate_texture_steps & 1) != 0;
        const float source_w = float(swap ? source->height : source->width);
        const float source_h = float(swap ? source->width : source->height);
        const float scale = std::min(float(config.width) / source_w, float(config.height) / source_h);
        const float content_w = source_w * scale;
        const float content_h = source_h * scale;
        p.content_offset[0] = (float(config.width) - content_w) * 0.5f;
        p.content_offset[1] = (float(config.height) - content_h) * 0.5f;
        p.content_scale[0] = 1.0f / content_w;
        p.content_scale[1] = 1.0f / content_h;
      }
      if (!params_valid || std::memcmp(&p, &last_params, sizeof(p)) != 0) {
        context->UpdateSubresource(params.Get(), 0, nullptr, &p, 0, 0);
        last_params = p;
        params_valid = true;
      }
    }

    result_e run_dct(const pyrowave::d3d11::source_t &source) {
      const bool blank = source.srv == nullptr;
      auto *shader = dct_shader_for(blank ? transfer_e::unorm : source.transfer);
      if (!shader) {
        return result_e::failed;
      }
      update_params(blank ? nullptr : &source);

      // Synchronize with the capture side, with a finite timeout so display re-init or
      // device loss cannot wedge the stream.
      if (!blank && source.mutex) {
        const HRESULT status = source.mutex->AcquireSync(0, static_cast<DWORD>(config.capture_wait.count()));
        if (status == static_cast<HRESULT>(WAIT_TIMEOUT)) {
          warning("timed out acquiring the capture mutex; skipping the frame");
          return result_e::skipped;
        }
        if (status == static_cast<HRESULT>(WAIT_ABANDONED)) {
          error("the capture mutex was abandoned; its texture must be recreated");
          return result_e::failed;
        }
        if (status != S_OK) {
          warning("acquiring the capture mutex failed [" + hresult_string(status) + "]; skipping the frame");
          return result_e::skipped;
        }
      }

      // The coefficient pass ORs int16 values into zeroed memory.
      const UINT zero[4] = {};
      context->ClearUnorderedAccessViewUint(coeffs_uav.Get(), zero);

      ID3D11Buffer *cbs[] = {color_matrix.Get(), sdr_to_pq_params.Get(), params.Get()};
      ID3D11ShaderResourceView *srvs[] = {source.srv, mb_info_srv.Get()};
      ID3D11UnorderedAccessView *uavs[] = {coeffs_uav.Get()};
      ID3D11SamplerState *samplers[] = {sampler.Get()};
      context->CSSetShader(shader, nullptr, 0);
      context->CSSetConstantBuffers(0, 3, cbs);
      context->CSSetShaderResources(0, 2, srvs);
      context->CSSetSamplers(0, 1, samplers);
      context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
      context->Dispatch(UINT(layout.mb_width), UINT(layout.mb_height), 1);

      ID3D11ShaderResourceView *null_srvs[2] = {};
      ID3D11UnorderedAccessView *null_uavs[1] = {};
      context->CSSetShaderResources(0, 2, null_srvs);
      context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);

      // The frame is in the coefficients as far as D3D11 ordering goes, so the capture
      // side can have its texture back.
      if (!blank && source.mutex) {
        source.mutex->ReleaseSync(0);
      }
      return result_e::ok;
    }
  };

  core_t::core_t() = default;
  core_t::~core_t() = default;

  std::unique_ptr<core_t> core_t::create(IDXGIAdapter *adapter, const core_config_t &config, log_fn log) {
    auto core = std::unique_ptr<core_t>(new core_t());
    core->impl = std::make_unique<impl_t>();
    auto &impl = *core->impl;
    impl.config = config;
    impl.log = std::move(log);

    if (config.width <= 0 || config.height <= 0 || config.width > 65535 || config.height > 65535) {
      impl.error("invalid encode size " + std::to_string(config.width) + "x" + std::to_string(config.height));
      return nullptr;
    }
    impl.layout = prores::layout_t::make(config.width, config.height, config.yuv444);

    if (!impl.create_device(adapter) || !impl.create_buffers() || !impl.create_constants()) {
      return nullptr;
    }

    // Compile the common SDR variant up front so a shader error ends session setup
    // instead of the first streamed frame.
    if (!impl.dct_shader_for(transfer_e::unorm)) {
      return nullptr;
    }
    return core;
  }

  ID3D11Device *core_t::device() const {
    return impl->device.Get();
  }

  const prores::layout_t &core_t::layout() const {
    return impl->layout;
  }

  result_e core_t::encode(
    const pyrowave::d3d11::source_t &source,
    std::size_t max_frame_bytes,
    std::vector<std::uint8_t> &out,
    std::size_t &critical_bytes,
    frame_stats_t *stats
  ) {
    (void) max_frame_bytes;
    out.clear();
    critical_bytes = 0;
    if (stats) {
      *stats = {};
    }
    const result_e result = impl->run_dct(source);
    return result == result_e::ok ? result_e::skipped : result;
  }

  bool core_t::read_coefficients(std::vector<std::int16_t> &coeffs) {
    auto &state = *impl;
    D3D11_BUFFER_DESC bd {};
    state.coeffs->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.MiscFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    if (FAILED(state.device->CreateBuffer(&bd, nullptr, &staging))) {
      state.error("staging buffer creation failed");
      return false;
    }
    state.context->CopyResource(staging.Get(), state.coeffs.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    const HRESULT hr = state.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
      state.error("mapping the coefficients failed [" + hresult_string(hr) + "]");
      return false;
    }
    coeffs.resize(state.layout.total_coeffs);
    std::memcpy(coeffs.data(), mapped.pData, coeffs.size() * sizeof(std::int16_t));
    state.context->Unmap(staging.Get(), 0);
    return true;
  }

  bool probe(const LUID *luid, const std::wstring &shader_dir, std::string &detail) {
    ComPtr<IDXGIAdapter> adapter;
    if (luid) {
      ComPtr<IDXGIFactory1> factory;
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        detail = "CreateDXGIFactory1 failed";
        return false;
      }
      for (UINT i = 0; SUCCEEDED(factory->EnumAdapters(i, &adapter)); i++) {
        DXGI_ADAPTER_DESC desc {};
        if (SUCCEEDED(adapter->GetDesc(&desc)) && desc.AdapterLuid.LowPart == luid->LowPart && desc.AdapterLuid.HighPart == luid->HighPart) {
          break;
        }
        adapter.Reset();
      }
      if (!adapter) {
        detail = "no adapter matches the LUID";
        return false;
      }
    }

    bool ok = true;
    detail.clear();
    auto log = [&](int level, const std::string &text) {
      if (level >= 2) {
        ok = false;
        detail += text + "\n";
      }
    };
    for (bool yuv444 : {false, true}) {
      core_config_t config;
      config.width = 64;
      config.height = 64;
      config.yuv444 = yuv444;
      config.shader_dir = shader_dir;
      auto core = core_t::create(adapter.Get(), config, log);
      if (!core) {
        return false;
      }
      for (auto transfer : {transfer_e::linear_sdr, transfer_e::pq, transfer_e::sdr_to_pq}) {
        if (!core->impl->dct_shader_for(transfer)) {
          return false;
        }
      }
    }
    return ok;
  }
}  // namespace prores::d3d11
