/**
 * @file tools/prores/prores_selftest.cpp
 * @brief CPU tests for the reference encoder (any OS). Exit code 0 means pass.
 */
#include "prores_bitstream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifndef GOLDEN_SHA
#define GOLDEN_SHA "8a8766678f8c7f38e82fa9d5c7b8655c9261ed60d01f8dfbf56d442df80b9309"  // 64x32 gradient, 4:2:2, q=4, 799 bytes; frozen after VideoToolbox decode check
#endif
static int failures = 0;
#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
    } \
  } while (0)

namespace {
  std::string sha256(const std::vector<std::uint8_t> &d) {
    static const std::uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> m = d;
    m.push_back(0x80);
    while (m.size() % 64 != 56) {
      m.push_back(0);
    }
    const std::uint64_t bits = (std::uint64_t) d.size() * 8;
    for (int i = 7; i >= 0; i--) {
      m.push_back((std::uint8_t) (bits >> (i * 8)));
    }
    auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t o = 0; o < m.size(); o += 64) {
      std::uint32_t w[64];
      for (int i = 0; i < 16; i++) {
        w[i] = (std::uint32_t) m[o + i * 4] << 24 | m[o + i * 4 + 1] << 16 | m[o + i * 4 + 2] << 8 | m[o + i * 4 + 3];
      }
      for (int i = 16; i < 64; i++) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      std::uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
      for (int i = 0; i < 64; i++) {
        std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = dd + t1; dd = c; c = b; b = a; a = t1 + t2;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char buf[65];
    for (int i = 0; i < 8; i++) {
      std::snprintf(buf + i * 8, 9, "%08x", h[i]);
    }
    return buf;
  }

  struct image_t {
    prores::layout_t layout;
    std::vector<std::uint16_t> data;
    const std::uint16_t *planes[3];
    std::size_t strides[3];
    std::vector<std::int16_t> coeffs;
    image_t(int w, int h, bool yuv444, int mbs = 8):
        layout(prores::layout_t::make(w, h, yuv444, mbs)) {
      const int cw = layout.plane_width(1);
      data.resize((std::size_t) w * h + 2 * (std::size_t) cw * h);
      planes[0] = data.data();
      planes[1] = planes[0] + (std::size_t) w * h;
      planes[2] = planes[1] + (std::size_t) cw * h;
      strides[0] = (std::size_t) w;
      strides[1] = strides[2] = (std::size_t) cw;
      coeffs.resize(layout.total_coeffs);
    }
    void encode() {
      prores::forward_dct(layout, planes, strides, coeffs.data());
    }
  };

  void fill_gradient(image_t &im) {
    const int w = im.layout.width, h = im.layout.height, cw = im.layout.plane_width(1);
    auto *d = const_cast<std::uint16_t *>(im.data.data());
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        d[(std::size_t) y * w + x] = (std::uint16_t) (64 + (x * 876 / w + y * 100 / h) % 877);
      }
      for (int x = 0; x < cw; x++) {
        d[(std::size_t) w * h + (std::size_t) y * cw + x] = (std::uint16_t) (64 + (x * 5 + y * 3) % 897);
        d[(std::size_t) w * h + (std::size_t) cw * h + (std::size_t) y * cw + x] = (std::uint16_t) (960 - (x * 7 + y * 2) % 897);
      }
    }
  }

  void fill_random(image_t &im, unsigned seed) {
    std::mt19937 rng(seed);
    for (auto &v : im.data) {
      v = (std::uint16_t) (rng() % 1024);
    }
  }

  void test_layout() {
    auto l = prores::layout_t::make(3024, 1890, false);
    CHECK(l.mb_width == 189 && l.mb_height == 119);
    CHECK(l.slices_per_row == 25);
    CHECK(l.slices.size() == 2975);
    for (int i = 0; i < 25; i++) {
      CHECK(l.slices[i].mbs == (i < 23 ? 8 : (i == 23 ? 4 : 1)));
    }
    CHECK(l.slices[25].mb_y == 1 && l.slices[25].mb_x == 0);
    CHECK(l.slices[23].mb_x == 184 && l.slices[24].mb_x == 188);
    CHECK(l.critical_bytes() == 36 + 2 * 2975);

    auto a = prores::layout_t::make(16, 16, false);
    CHECK(a.slices.size() == 1 && a.slices[0].mbs == 1 && a.total_coeffs == (4 + 2 + 2) * 64);
    auto b = prores::layout_t::make(17, 17, false);
    CHECK(b.mb_width == 2 && b.mb_height == 2 && b.slices.size() == 2 && b.slices[0].mbs == 2);
    auto c = prores::layout_t::make(1920, 1080, false);
    CHECK(c.mb_width == 120 && c.mb_height == 68 && c.slices_per_row == 15 && c.slices.size() == 68 * 15);  // 120 = 15 * 8, no remainder
    auto d = prores::layout_t::make(1000, 333, true);  // 63 MBs: 7 * 8 + (4 + 2 + 1)
    CHECK(d.mb_width == 63 && d.slices_per_row == 10 && d.blocks_per_mb(1) == 4);
    CHECK(d.total_coeffs == (std::uint32_t) d.mb_width * d.mb_height * 12 * 64);
  }

  void test_flat() {
    image_t im(16, 16, false);
    for (auto &v : im.data) {
      v = 512;
    }
    im.encode();
    const auto &sl = im.layout.slices[0];
    for (int p = 0; p < 3; p++) {
      const int bps = sl.mbs * im.layout.blocks_per_mb(p);
      for (int b = 0; b < bps; b++) {
        CHECK(im.coeffs[sl.coeff_offset[p] + b] == 16384);
        for (int pos = 1; pos < 64; pos++) {
          CHECK(im.coeffs[sl.coeff_offset[p] + (std::size_t) pos * bps + b] == 0);
        }
      }
    }
  }

  void check_sizes(image_t &im, const char *name) {
    prores::quant_t qm;
    prores::color_t color;
    for (int q : prores::default_ladder) {
      std::vector<std::uint8_t> out;
      prores::write_frame(im.layout, im.coeffs.data(), q, color, qm, out);
      prores::parsed_t pr;
      CHECK(prores::parse_frame(out, pr));
      CHECK(pr.slice_sizes.size() == im.layout.slices.size());
      std::size_t sum = 0;
      for (std::size_t i = 0; i < pr.slice_sizes.size(); i++) {
        const std::uint8_t *s = out.data() + pr.slice_offsets[i];
        const std::size_t y = s[2] << 8 | s[3], cb = s[4] << 8 | s[5];
        const std::size_t cr = pr.slice_sizes[i] - 6 - y - cb;
        CHECK(s[1] == q);
        CHECK(y == prores::plane_bytes(im.layout, im.coeffs.data(), i, 0, q, qm));
        CHECK(cb == prores::plane_bytes(im.layout, im.coeffs.data(), i, 1, q, qm));
        CHECK(cr == prores::plane_bytes(im.layout, im.coeffs.data(), i, 2, q, qm));
        sum += pr.slice_sizes[i];
      }
      CHECK(prores::frame_bytes(im.layout, im.coeffs.data(), q, qm) == out.size());
      CHECK(8 + 20 + 8 + 2 * pr.slice_sizes.size() + sum == out.size());
      if (failures) {
        std::printf("  (%s q=%d)\n", name, q);
        return;
      }
    }
  }

  void test_sizes() {
    for (bool d444 : {false, true}) {
      image_t r(100, 40, d444);  // 7 MBs wide: slices 4, 2, 1
      fill_random(r, 7);
      r.encode();
      check_sizes(r, "random");
      image_t g(176, 48, d444);
      fill_gradient(g);
      g.encode();
      check_sizes(g, "gradient");
    }
  }

  void test_roundtrip_header() {
    image_t im(64, 32, false);
    fill_gradient(im);
    im.encode();
    prores::quant_t qm;
    qm.send = true;
    qm.luma[5] = 9;
    std::vector<std::uint8_t> out;
    prores::color_t color {9, 16, 9};
    prores::write_frame(im.layout, im.coeffs.data(), 8, color, qm, out);
    prores::parsed_t pr;
    CHECK(prores::parse_frame(out, pr));
    CHECK(pr.width == 64 && pr.height == 32 && !pr.yuv444);
    CHECK(out[22] == 9 && out[23] == 16 && out[24] == 9 && out[27] == 3);
    CHECK(out[8] == 0 && out[9] == 148 && out[28 + 5] == 9);
    CHECK(pr.picture_offset == 8 + 148);
    CHECK(out[pr.picture_offset + 7] == 0x30);
    // truncated and corrupted frames are rejected
    auto cut = std::vector<std::uint8_t>(out.begin(), out.end() - 1);
    CHECK(!prores::parse_frame(cut, pr));
  }

  void test_budget() {
    image_t im(176, 48, false);
    fill_random(im, 3);
    im.encode();
    prores::quant_t qm;
    std::size_t prev = ~(std::size_t) 0;
    for (int i = 0; i < 16; i++) {
      const std::size_t sz = prores::frame_bytes(im.layout, im.coeffs.data(), prores::default_ladder[i], qm);
      CHECK(sz <= prev);  // size shrinks as q grows
      prev = sz;
    }
    const std::size_t mid = prores::frame_bytes(im.layout, im.coeffs.data(), prores::default_ladder[7], qm);
    CHECK(prores::choose_ladder_index(im.layout, im.coeffs.data(), mid, prores::default_ladder, qm) == 7);
    CHECK(prores::choose_ladder_index(im.layout, im.coeffs.data(), 1, prores::default_ladder, qm) == 15);
    CHECK(prores::choose_ladder_index(im.layout, im.coeffs.data(), 1u << 30, prores::default_ladder, qm) == 0);
  }

  void test_golden() {
    image_t im(64, 32, false);
    fill_gradient(im);
    im.encode();
    prores::quant_t qm;
    std::vector<std::uint8_t> out;
    prores::write_frame(im.layout, im.coeffs.data(), 4, prores::color_t {}, qm, out);
    const std::string got = sha256(out);
    const char *want = GOLDEN_SHA;
    if (std::strcmp(want, "unset") == 0) {
      std::printf("golden sha256 = %s (%zu bytes)\n", got.c_str(), out.size());
    } else {
      CHECK(got == want);
    }
  }
}  // namespace

int main() {
  CHECK(sha256({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  test_layout();
  test_flat();
  test_sizes();
  test_roundtrip_header();
  test_budget();
  test_golden();
  std::printf(failures ? "FAILED (%d)\n" : "all tests passed\n", failures);
  return failures ? 1 : 0;
}
