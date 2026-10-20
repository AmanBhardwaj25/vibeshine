/**
 * @file src/prores/prores_bitstream.cpp
 * @brief CPU reference ProRes encoder. Entropy coder ported from FFmpeg's proresenc_kostya.c (LGPL 2.1+).
 */
#include "prores_bitstream.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace prores {
  namespace {
    constexpr std::uint8_t scan[64] = {
      0, 1, 8, 9, 2, 3, 10, 11,
      16, 17, 24, 25, 18, 19, 26, 27,
      4, 5, 12, 20, 13, 6, 7, 14,
      21, 28, 29, 22, 15, 23, 30, 31,
      32, 33, 40, 48, 41, 34, 35, 42,
      49, 56, 57, 50, 43, 36, 37, 44,
      51, 58, 59, 52, 45, 38, 39, 46,
      53, 60, 61, 54, 47, 55, 62, 63
    };
    constexpr std::uint8_t dc_cb[7] = {0x04, 0x28, 0x28, 0x4D, 0x4D, 0x70, 0x70};
    constexpr std::uint8_t run_cb[16] = {0x06, 0x06, 0x05, 0x05, 0x04, 0x29, 0x29, 0x29, 0x29, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x4C};
    constexpr std::uint8_t level_cb[10] = {0x04, 0x0A, 0x05, 0x06, 0x04, 0x28, 0x28, 0x28, 0x28, 0x4C};
    constexpr int first_dc_cb = 0xB8;

    std::size_t frame_header_size(const quant_t &qm) {
      return qm.send ? 148 : 20;
    }

    // Bit sinks: the same coder runs against both, so counts and writes cannot disagree.
    struct count_sink_t {
      std::size_t bits = 0;
      void put(int n, std::uint32_t) {
        bits += n;
      }
    };

    struct write_sink_t {
      std::uint8_t *out;
      std::uint64_t acc = 0;
      int nacc = 0;
      explicit write_sink_t(std::uint8_t *p):
          out(p) {}
      void put(int n, std::uint32_t val) {
        // n <= 32 always holds for this coder (longest code is about 40 bits only for absurd levels)
        if (n <= 0) {
          return;
        }
        if (n > 32) {
          put(n - 32, 0);
          n = 32;
        }
        acc = (acc << n) | (n == 32 ? val : (val & ((1u << n) - 1)));
        nacc += n;
        while (nacc >= 8) {
          *out++ = (std::uint8_t) (acc >> (nacc - 8));
          nacc -= 8;
        }
      }
      void flush() {
        if (nacc) {
          *out++ = (std::uint8_t) (acc << (8 - nacc));
          nacc = 0;
        }
      }
    };

    template<class S>
    void codeword(S &s, unsigned cb, int val) {
      unsigned switch_bits = (cb & 3) + 1, rice = cb >> 5, expo = (cb >> 2) & 7;
      unsigned switch_val = switch_bits << rice;
      if ((unsigned) val >= switch_val) {
        val -= (int) (switch_val - (1u << expo));
        int e = 31 - std::countl_zero((unsigned) val);
        s.put(e - (int) expo + (int) switch_bits, 0);
        s.put(e + 1, (std::uint32_t) val);
      } else {
        int e = val >> rice;
        if (e) {
          s.put(e, 0);
        }
        s.put(1, 1);
        if (rice) {
          s.put((int) rice, (std::uint32_t) val);
        }
      }
    }

    inline int make_code(int x) {
      return (x * 2) ^ (x >> 31);
    }

    template<class S>
    void code_plane(S &s, const std::int16_t *c, int blocks, const std::uint8_t *qm, int q) {
      int scale = qm[0] * q;
      int prev = (c[0] - 0x4000) / scale;
      codeword(s, first_dc_cb, make_code(prev));
      int sign = 0, cbi = 5;
      for (int b = 1; b < blocks; b++) {
        int dc = (c[b] - 0x4000) / scale;
        int d = dc - prev;
        int ns = d >> 31;
        d = (d ^ sign) - sign;
        int code = make_code(d);
        codeword(s, dc_cb[cbi], code);
        cbi = std::min(code, 6);
        sign = ns;
        prev = dc;
      }
      int prev_run = 4, prev_level = 2, run = 0;
      for (int p = 1; p < 64; p++) {
        const int div = qm[scan[p]] * q;
        const std::int16_t *row = c + (std::size_t) p * blocks;
        for (int b = 0; b < blocks; b++) {
          int level = row[b] / div;
          if (level) {
            int al = level < 0 ? -level : level;
            codeword(s, run_cb[prev_run], run);
            codeword(s, level_cb[prev_level], al - 1);
            s.put(1, level < 0 ? 1 : 0);
            prev_run = std::min(run, 15);
            prev_level = std::min(al, 9);
            run = 0;
          } else {
            run++;
          }
        }
      }
    }

    int blocks_in(const layout_t &l, const slice_t &sl, int plane) {
      return sl.mbs * l.blocks_per_mb(plane);
    }

    void put16(std::uint8_t *p, unsigned v) {
      p[0] = (std::uint8_t) (v >> 8);
      p[1] = (std::uint8_t) v;
    }

    void put32(std::uint8_t *p, std::uint32_t v) {
      put16(p, v >> 16);
      put16(p + 2, v & 0xFFFF);
    }

    const std::uint16_t *plane_ptr(const std::uint16_t *base, std::size_t stride, int x, int y, int pw, int ph) {
      x = std::clamp(x, 0, pw - 1);
      y = std::clamp(y, 0, ph - 1);
      return base + (std::size_t) y * stride + x;
    }

    // Orthonormal 8-point DCT-II basis, c[k][n].
    struct dct_table_t {
      double c[8][8];
      dct_table_t() {
        for (int k = 0; k < 8; k++) {
          for (int n = 0; n < 8; n++) {
            c[k][n] = std::sqrt(k == 0 ? 1.0 / 8 : 2.0 / 8) * std::cos((2 * n + 1) * k * 3.14159265358979323846 / 16);
          }
        }
      }
    };
  }  // namespace

  layout_t layout_t::make(int width, int height, bool yuv444, int mbs_per_slice) {
    layout_t l;
    l.width = width;
    l.height = height;
    l.yuv444 = yuv444;
    l.mbs_per_slice = mbs_per_slice;
    l.mb_width = (width + 15) / 16;
    l.mb_height = (height + 15) / 16;
    std::vector<int> sizes;
    int x = 0;
    for (int n = l.mb_width; n > 0;) {
      int s = mbs_per_slice;
      while (s > n) {
        s >>= 1;
      }
      sizes.push_back(s);
      n -= s;
    }
    l.slices_per_row = (int) sizes.size();
    std::uint32_t off[3] = {0, 0, 0};
    for (int y = 0; y < l.mb_height; y++) {
      x = 0;
      for (int s : sizes) {
        slice_t sl {(std::uint16_t) x, (std::uint16_t) y, (std::uint16_t) s, {off[0], off[1], off[2]}};
        l.slices.push_back(sl);
        for (int p = 0; p < 3; p++) {
          off[p] += (std::uint32_t) (s * l.blocks_per_mb(p) * 64);
        }
        x += s;
      }
    }
    // Planes are stored back to back: make every slice offset global.
    const std::uint32_t base[3] = {0, off[0], off[0] + off[1]};
    for (auto &sl : l.slices) {
      for (int p = 0; p < 3; p++) {
        sl.coeff_offset[p] += base[p];
      }
    }
    l.total_coeffs = off[0] + off[1] + off[2];
    return l;
  }

  int layout_t::blocks_per_mb(int plane) const {
    return (plane == 0 || yuv444) ? 4 : 2;
  }

  std::size_t layout_t::critical_bytes() const {
    return 8 + 20 + 8 + 2 * slices.size();
  }

  int layout_t::plane_width(int plane) const {
    return (plane == 0 || yuv444) ? width : (width + 1) / 2;
  }

  void forward_dct(const layout_t &l, const std::uint16_t *const planes[3], const std::size_t strides[3], std::int16_t *coeffs) {
    static const dct_table_t T;
    for (const auto &sl : l.slices) {
      for (int p = 0; p < 3; p++) {
        const int bpm = l.blocks_per_mb(p);
        const int bps = sl.mbs * bpm;
        const int pw = l.plane_width(p), ph = l.height;
        const bool chroma422 = p > 0 && !l.yuv444;
        std::int16_t *dst = coeffs + sl.coeff_offset[p];
        for (int mbi = 0; mbi < sl.mbs; mbi++) {
          const int mbx = sl.mb_x + mbi;
          for (int i = 0; i < bpm; i++) {
            int bx, by;
            if (p == 0) {
              bx = i & 1;
              by = i >> 1;
            } else if (chroma422) {
              bx = 0;
              by = i;
            } else {
              bx = i >> 1;
              by = i & 1;
            }
            const int x0 = (chroma422 ? mbx * 8 : mbx * 16) + bx * 8;
            const int y0 = sl.mb_y * 16 + by * 8;
            double in[8][8], tmp[8][8];
            for (int r = 0; r < 8; r++) {
              for (int c = 0; c < 8; c++) {
                in[r][c] = *plane_ptr(planes[p], strides[p], x0 + c, y0 + r, pw, ph);
              }
            }
            for (int r = 0; r < 8; r++) {
              for (int k = 0; k < 8; k++) {
                double a = 0;
                for (int n = 0; n < 8; n++) {
                  a += T.c[k][n] * in[r][n];
                }
                tmp[r][k] = a;
              }
            }
            const int b = mbi * bpm + i;
            for (int k = 0; k < 8; k++) {
              for (int c = 0; c < 8; c++) {
                double a = 0;
                for (int n = 0; n < 8; n++) {
                  a += T.c[k][n] * tmp[n][c];
                }
                const int raster = k * 8 + c;
                // storage position is the scan position of this raster index
                int pos = 0;
                while (scan[pos] != raster) {
                  pos++;
                }
                dst[(std::size_t) pos * bps + b] = (std::int16_t) (int) std::trunc(4 * a);
              }
            }
          }
        }
      }
    }
  }

  std::size_t plane_bytes(const layout_t &l, const std::int16_t *coeffs, std::size_t slice, int plane, int q, const quant_t &qm) {
    const auto &sl = l.slices[slice];
    count_sink_t s;
    code_plane(s, coeffs + sl.coeff_offset[plane], blocks_in(l, sl, plane), plane ? qm.chroma : qm.luma, q);
    return (s.bits + 7) / 8;
  }

  namespace {
    // Sizes of every plane of every slice at q.
    void all_sizes(const layout_t &l, const std::int16_t *coeffs, int q, const quant_t &qm, std::vector<std::uint32_t> &sizes) {
      sizes.resize(l.slices.size() * 3);
      for (std::size_t i = 0; i < l.slices.size(); i++) {
        const auto &sl = l.slices[i];
        for (int p = 0; p < 3; p++) {
          count_sink_t s;
          code_plane(s, coeffs + sl.coeff_offset[p], blocks_in(l, sl, p), p ? qm.chroma : qm.luma, q);
          sizes[i * 3 + p] = (std::uint32_t) ((s.bits + 7) / 8);
        }
      }
    }

    std::size_t total_from(const layout_t &l, const std::vector<std::uint32_t> &sizes, const quant_t &qm) {
      std::size_t t = 8 + frame_header_size(qm) + 8 + 2 * l.slices.size();
      for (std::size_t i = 0; i < l.slices.size(); i++) {
        t += 6 + sizes[i * 3] + sizes[i * 3 + 1] + sizes[i * 3 + 2];
      }
      return t;
    }
  }  // namespace

  std::size_t frame_bytes(const layout_t &l, const std::int16_t *coeffs, int q, const quant_t &qm) {
    std::vector<std::uint32_t> sizes;
    all_sizes(l, coeffs, q, qm, sizes);
    return total_from(l, sizes, qm);
  }

  int choose_ladder_index(const layout_t &l, const std::int16_t *coeffs, std::size_t budget, const std::array<int, 16> &ladder, const quant_t &qm) {
    std::vector<std::uint32_t> sizes;
    for (int i = 0; i < 16; i++) {
      all_sizes(l, coeffs, ladder[i], qm, sizes);
      if (total_from(l, sizes, qm) <= budget) {
        return i;
      }
    }
    return 15;
  }

  std::size_t write_frame(const layout_t &l, const std::int16_t *coeffs, int q, const color_t &color, const quant_t &qm, std::vector<std::uint8_t> &out) {
    std::vector<std::uint32_t> sizes;
    all_sizes(l, coeffs, q, qm, sizes);
    const std::size_t total = total_from(l, sizes, qm);
    const std::size_t hdr = frame_header_size(qm);
    out.assign(total, 0);
    std::uint8_t *f = out.data();
    put32(f, (std::uint32_t) total);
    std::memcpy(f + 4, "icpf", 4);
    put16(f + 8, (unsigned) hdr);
    put16(f + 10, l.yuv444 ? 1 : 0);
    std::memcpy(f + 12, "cfrm", 4);
    put16(f + 16, (unsigned) l.width);
    put16(f + 18, (unsigned) l.height);
    f[20] = l.yuv444 ? 0xC0 : 0x80;
    f[22] = color.primaries;
    f[23] = color.transfer;
    f[24] = color.matrix;
    f[27] = qm.send ? 0x03 : 0;
    if (qm.send) {
      std::memcpy(f + 28, qm.luma, 64);
      std::memcpy(f + 92, qm.chroma, 64);
    }
    std::uint8_t *pic = f + 8 + hdr;
    const std::size_t nsl = l.slices.size();
    pic[0] = 0x40;
    put32(pic + 1, (std::uint32_t) (total - 8 - hdr));
    put16(pic + 5, (unsigned) nsl);
    pic[7] = (std::uint8_t) (std::countr_zero((unsigned) l.mbs_per_slice) << 4);
    std::uint8_t *table = pic + 8;
    std::uint8_t *dst = table + 2 * nsl;
    for (std::size_t i = 0; i < nsl; i++) {
      const auto &sl = l.slices[i];
      std::uint8_t *start = dst;
      dst[0] = 0x30;
      dst[1] = (std::uint8_t) q;
      put16(dst + 2, sizes[i * 3]);
      put16(dst + 4, sizes[i * 3 + 1]);
      dst += 6;
      for (int p = 0; p < 3; p++) {
        write_sink_t s(dst);
        code_plane(s, coeffs + sl.coeff_offset[p], blocks_in(l, sl, p), p ? qm.chroma : qm.luma, q);
        s.flush();
        dst = s.out;
      }
      put16(table + 2 * i, (unsigned) (dst - start));
    }
    return out.size();
  }

  bool parse_frame(std::span<const std::uint8_t> f, parsed_t &o) {
    auto g16 = [&](std::size_t p) { return (unsigned) (f[p] << 8 | f[p + 1]); };
    auto g32 = [&](std::size_t p) { return ((std::uint32_t) g16(p) << 16) | g16(p + 2); };
    if (f.size() < 36 || g32(0) != f.size() || std::memcmp(f.data() + 4, "icpf", 4) != 0) {
      return false;
    }
    const std::size_t hdr = g16(8);
    if (hdr < 20 || 8 + hdr + 8 > f.size()) {
      return false;
    }
    o.width = (int) g16(16);
    o.height = (int) g16(18);
    o.yuv444 = (f[20] >> 6) == 3;
    o.picture_offset = 8 + hdr;
    const std::size_t pic = o.picture_offset;
    if (f[pic] != 0x40 || pic + g32(pic + 1) != f.size()) {
      return false;
    }
    const std::size_t n = g16(pic + 5);
    o.table_offset = pic + 8;
    o.data_offset = o.table_offset + 2 * n;
    if (o.data_offset > f.size()) {
      return false;
    }
    o.slice_offsets.clear();
    o.slice_sizes.clear();
    std::size_t off = o.data_offset;
    for (std::size_t i = 0; i < n; i++) {
      const unsigned sz = g16(o.table_offset + 2 * i);
      if (sz < 6 || off + sz > f.size() || f[off] != 0x30) {
        return false;
      }
      o.slice_offsets.push_back((std::uint32_t) off);
      o.slice_sizes.push_back(sz);
      off += sz;
    }
    return off == f.size();
  }
}  // namespace prores
