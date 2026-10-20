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
#include <bit>
#include <chrono>
#include <cstdio>
#include <cmath>
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
      std::uint32_t direct;  ///< 1:1 mapping: the shader loads texels instead of sampling
      std::uint32_t padding[2];
    };

    static_assert(sizeof(dct_params_t) % 16 == 0, "Constant buffers are sized in 16-byte units");

    // Matches prores_params (b0) in prores_common.hlsl.
    struct coder_params_t {
      std::uint32_t slices;
      std::uint32_t budget_bytes;
      std::uint32_t capacity_bytes;
      std::uint32_t chroma_blocks;
      std::uint32_t log2_mbs;
      std::uint32_t padding[3];
      std::uint32_t ladder[16];
      std::uint32_t qmat[128];  ///< luma then chroma, one uint per byte
    };

    static_assert(sizeof(coder_params_t) % 16 == 0, "Constant buffers are sized in 16-byte units");

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
    ComPtr<ID3D11Buffer> counts;
    ComPtr<ID3D11UnorderedAccessView> counts_uav;
    ComPtr<ID3D11UnorderedAccessView> plan_uav;
    ComPtr<ID3D11UnorderedAccessView> out_uav;
    ComPtr<ID3D11Buffer> out_buffer;
    ComPtr<ID3D11Buffer> staging;
    ComPtr<ID3D11Buffer> slice_info;
    ComPtr<ID3D11ShaderResourceView> slice_info_srv;
    ComPtr<ID3D11Buffer> coder_params;
    ComPtr<ID3D11ComputeShader> count_shader, choose_shader, write_shader;
    std::size_t frame_prefix = 0;  ///< frame atom + frame header
    std::size_t capacity_picture = 0;
    std::uint32_t out_bytes = 0;
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

      // Count, choose and write passes.
      const std::size_t slices = layout.slices.size();
      frame_prefix = 8 + (config.quant.send ? 148 : 20);
      const std::size_t capacity_frame = std::max(config.capacity_frame_bytes, layout.critical_bytes() + 1024);
      capacity_picture = capacity_frame - frame_prefix;
      out_bytes = UINT((16 + capacity_picture + 3) & ~std::size_t(3));
      auto raw_buffer = [&](UINT bytes, ComPtr<ID3D11Buffer> &buffer, ComPtr<ID3D11UnorderedAccessView> &view, const char *what) {
        D3D11_BUFFER_DESC rd {};
        rd.ByteWidth = bytes;
        rd.Usage = D3D11_USAGE_DEFAULT;
        rd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        rd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        HRESULT result = device->CreateBuffer(&rd, nullptr, &buffer);
        if (SUCCEEDED(result)) {
          D3D11_UNORDERED_ACCESS_VIEW_DESC rv {};
          rv.Format = DXGI_FORMAT_R32_TYPELESS;
          rv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
          rv.Buffer.NumElements = bytes / 4;
          rv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
          result = device->CreateUnorderedAccessView(buffer.Get(), &rv, &view);
        }
        if (FAILED(result)) {
          error(std::string(what) + " buffer creation failed [" + hresult_string(result) + "]");
          return false;
        }
        return true;
      };
      ComPtr<ID3D11Buffer> plan;
      if (!raw_buffer(UINT(16 * slices * 3 * 4), counts, counts_uav, "count") || !raw_buffer(UINT((slices + 1) * 16), plan, plan_uav, "plan") ||
          !raw_buffer(out_bytes, out_buffer, out_uav, "output")) {
        return false;
      }
      bd = {};
      bd.ByteWidth = out_bytes;
      bd.Usage = D3D11_USAGE_STAGING;
      bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      hr = device->CreateBuffer(&bd, nullptr, &staging);
      if (FAILED(hr)) {
        error("readback buffer creation failed [" + hresult_string(hr) + "]");
        return false;
      }

      // Per slice: mbs, coefficient offsets of the three planes.
      std::vector<std::uint32_t> slice_data(slices * 4);
      for (std::size_t i = 0; i < slices; i++) {
        const auto &slice = layout.slices[i];
        slice_data[i * 4] = slice.mbs;
        for (int p = 0; p < 3; p++) {
          slice_data[i * 4 + 1 + p] = slice.coeff_offset[p];
        }
      }
      bd = {};
      bd.ByteWidth = UINT(slice_data.size() * sizeof(std::uint32_t));
      bd.Usage = D3D11_USAGE_IMMUTABLE;
      bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      bd.StructureByteStride = 16;
      init = {slice_data.data(), 0, 0};
      hr = device->CreateBuffer(&bd, &init, &slice_info);
      if (SUCCEEDED(hr)) {
        sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = UINT(slices);
        hr = device->CreateShaderResourceView(slice_info.Get(), &sd, &slice_info_srv);
      }
      if (FAILED(hr)) {
        error("slice table creation failed [" + hresult_string(hr) + "]");
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

      cb.ByteWidth = sizeof(coder_params_t);
      cb.Usage = D3D11_USAGE_DEFAULT;
      hr = device->CreateBuffer(&cb, nullptr, &coder_params);
      if (FAILED(hr)) {
        error("coder parameter buffer creation failed [" + hresult_string(hr) + "]");
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

      shader = compile("prores_dct_cs.hlsl", macros.data());
      return shader.Get();
    }

    ComPtr<ID3D11ComputeShader> compile(const char *file, const D3D_SHADER_MACRO *macros) {
      const std::wstring path = config.shader_dir + L"/" + std::wstring(file, file + std::strlen(file));
      ComPtr<ID3DBlob> code;
      ComPtr<ID3DBlob> messages;
      HRESULT hr = D3DCompileFromFile(path.c_str(), macros, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main_cs", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
      if (FAILED(hr)) {
        std::string detail = messages ? std::string(static_cast<const char *>(messages->GetBufferPointer()), messages->GetBufferSize()) : std::string("no compiler output");
        error(std::string("compiling ") + file + " failed [" + hresult_string(hr) + "]: " + detail);
        return nullptr;
      }
      ComPtr<ID3D11ComputeShader> shader;
      hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
      if (FAILED(hr)) {
        error(std::string("CreateComputeShader failed for ") + file + " [" + hresult_string(hr) + "]");
        return nullptr;
      }
      return shader;
    }

    bool compile_coder_shaders() {
      count_shader = compile("prores_count_cs.hlsl", nullptr);
      choose_shader = compile("prores_choose_cs.hlsl", nullptr);
      write_shader = compile("prores_write_cs.hlsl", nullptr);
      return count_shader && choose_shader && write_shader;
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
        p.direct = (source->rotate_texture_steps & 3) == 0 && content_w == source_w && content_h == source_h &&
                   p.content_offset[0] == std::floor(p.content_offset[0]) && p.content_offset[1] == std::floor(p.content_offset[1]);
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

    // Count, choose and write passes, then the single blocking readback. Returns the picture
    // bytes (0: the frame does not fit the capacity) in `picture` and the chosen q.
    result_e run_coder(std::size_t max_frame_bytes, std::vector<std::uint8_t> &out, std::size_t &picture, int &q) {
      const std::size_t slices = layout.slices.size();
      const std::size_t budget = max_frame_bytes > frame_prefix ? max_frame_bytes - frame_prefix : 0;

      coder_params_t cp {};
      cp.slices = std::uint32_t(slices);
      cp.budget_bytes = std::uint32_t(std::min<std::size_t>(budget, 0xFFFFFFFFu));
      cp.capacity_bytes = std::uint32_t(capacity_picture);
      cp.chroma_blocks = config.yuv444 ? 4 : 2;
      cp.log2_mbs = std::uint32_t(std::countr_zero(unsigned(layout.mbs_per_slice)));
      for (int i = 0; i < 16; i++) {
        cp.ladder[i] = std::uint32_t(config.ladder[i]);
      }
      for (int i = 0; i < 64; i++) {
        cp.qmat[i] = config.quant.luma[i];
        cp.qmat[64 + i] = config.quant.chroma[i];
      }
      context->UpdateSubresource(coder_params.Get(), 0, nullptr, &cp, 0, 0);

      const UINT zero[4] = {};
      context->ClearUnorderedAccessViewUint(out_uav.Get(), zero);

      ID3D11Buffer *cbs[] = {coder_params.Get()};
      ID3D11ShaderResourceView *srvs[] = {slice_info_srv.Get()};
      ID3D11UnorderedAccessView *uavs[] = {coeffs_uav.Get(), counts_uav.Get(), plan_uav.Get(), out_uav.Get()};
      context->CSSetConstantBuffers(0, 1, cbs);
      context->CSSetShaderResources(0, 1, srvs);
      context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
      const UINT groups = UINT((slices + 63) / 64);
      context->CSSetShader(count_shader.Get(), nullptr, 0);
      context->Dispatch(groups, 3, 16);
      context->CSSetShader(choose_shader.Get(), nullptr, 0);
      context->Dispatch(1, 1, 1);
      context->CSSetShader(write_shader.Get(), nullptr, 0);
      context->Dispatch(groups, 3, 1);

      ID3D11ShaderResourceView *null_srvs[1] = {};
      ID3D11UnorderedAccessView *null_uavs[4] = {};
      context->CSSetShaderResources(0, 1, null_srvs);
      context->CSSetUnorderedAccessViews(0, 4, null_uavs, nullptr);

      auto read_back = [&](std::size_t bytes, D3D11_MAPPED_SUBRESOURCE &mapped) {
        bytes = std::min<std::size_t>((bytes + 3) & ~std::size_t(3), out_bytes);
        const D3D11_BOX box {0, 0, 0, UINT(bytes), 1, 1};
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, out_buffer.Get(), 0, &box);
        const HRESULT hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
          error("mapping the picture failed [" + hresult_string(hr) + "]");
          return false;
        }
        return true;
      };

      const std::size_t bound = 16 + std::min(budget, capacity_picture);
      D3D11_MAPPED_SUBRESOURCE mapped {};
      if (!read_back(bound, mapped)) {
        return result_e::failed;
      }
      std::uint32_t result[4];
      std::memcpy(result, mapped.pData, sizeof(result));
      picture = result[0];
      q = int(result[1]);
      if (picture != 0 && 16 + picture > bound) {
        // Only the coarsest q was left and it exceeds the budget: fetch the rest.
        context->Unmap(staging.Get(), 0);
        if (!read_back(16 + picture, mapped)) {
          return result_e::failed;
        }
      }
      if (picture != 0) {
        const std::size_t total = frame_prefix + picture;
        out.resize(total);
        std::uint8_t *f = out.data();
        std::memset(f, 0, frame_prefix);
        auto put16 = [](std::uint8_t *d, unsigned v) {
          d[0] = std::uint8_t(v >> 8);
          d[1] = std::uint8_t(v);
        };
        put16(f, unsigned(total >> 16));
        put16(f + 2, unsigned(total & 0xFFFF));
        std::memcpy(f + 4, "icpf", 4);
        put16(f + 8, unsigned(frame_prefix - 8));
        put16(f + 10, config.yuv444 ? 1 : 0);
        std::memcpy(f + 12, "cfrm", 4);
        put16(f + 16, unsigned(config.width));
        put16(f + 18, unsigned(config.height));
        f[20] = config.yuv444 ? 0xC0 : 0x80;
        f[22] = config.color.primaries;
        f[23] = config.color.transfer;
        f[24] = config.color.matrix;
        f[27] = config.quant.send ? 0x03 : 0;
        if (config.quant.send) {
          std::memcpy(f + 28, config.quant.luma, 64);
          std::memcpy(f + 92, config.quant.chroma, 64);
        }
        std::memcpy(f + frame_prefix, static_cast<const std::uint8_t *>(mapped.pData) + 16, picture);
      }
      context->Unmap(staging.Get(), 0);
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
    if (!impl.dct_shader_for(transfer_e::unorm) || !impl.compile_coder_shaders()) {
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
    out.clear();
    critical_bytes = 0;
    if (stats) {
      *stats = {};
    }
    const auto start = std::chrono::steady_clock::now();
    result_e result = impl->run_dct(source);
    if (result != result_e::ok) {
      return result;
    }
    std::size_t picture = 0;
    int q = 0;
    result = impl->run_coder(max_frame_bytes, out, picture, q);
    if (result != result_e::ok) {
      return result;
    }
    if (picture == 0) {
      impl->warning("the frame does not fit the output capacity at the coarsest q; skipping it");
      return result_e::skipped;
    }
    critical_bytes = impl->frame_prefix + 8 + 2 * impl->layout.slices.size();
    if (stats) {
      stats->gpu_wait_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      stats->frame_bytes = out.size();
      stats->q = q;
    }
    return result_e::ok;
  }

  namespace {
    bool read_buffer(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Buffer *buffer, std::size_t bytes, void *dst) {
      D3D11_BUFFER_DESC bd {};
      buffer->GetDesc(&bd);
      bd.Usage = D3D11_USAGE_STAGING;
      bd.BindFlags = 0;
      bd.MiscFlags = 0;
      bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ComPtr<ID3D11Buffer> staging;
      if (FAILED(device->CreateBuffer(&bd, nullptr, &staging))) {
        return false;
      }
      context->CopyResource(staging.Get(), buffer);
      D3D11_MAPPED_SUBRESOURCE mapped {};
      if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
      }
      std::memcpy(dst, mapped.pData, bytes);
      context->Unmap(staging.Get(), 0);
      return true;
    }
  }  // namespace

  bool core_t::read_coefficients(std::vector<std::int16_t> &coeffs) {
    auto &state = *impl;
    coeffs.resize(state.layout.total_coeffs);
    if (!read_buffer(state.device.Get(), state.context.Get(), state.coeffs.Get(), coeffs.size() * sizeof(std::int16_t), coeffs.data())) {
      state.error("reading the coefficients back failed");
      return false;
    }
    return true;
  }

  bool core_t::read_counts(std::vector<std::uint32_t> &counts) {
    auto &state = *impl;
    counts.resize(16 * state.layout.slices.size() * 3);
    if (!read_buffer(state.device.Get(), state.context.Get(), state.counts.Get(), counts.size() * sizeof(std::uint32_t), counts.data())) {
      state.error("reading the counts back failed");
      return false;
    }
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
