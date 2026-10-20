/**
 * @file tools/prores/prores_ref_encode.cpp
 * @brief CLI: raw yuv422p10le / yuv444p10le frames in, concatenated ProRes frames out.
 *
 * prores_ref_encode in.yuv W H 422|444 q|budget N out.bin
 *   q N       fixed scale N (1..128)
 *   budget N  largest ladder entry that fits N bytes per frame
 */
#include "prores_bitstream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 8) {
    std::fprintf(stderr, "usage: %s in.yuv W H 422|444 q|budget N out.bin\n", argv[0]);
    return 2;
  }
  const int w = std::atoi(argv[2]), h = std::atoi(argv[3]);
  const bool yuv444 = std::strcmp(argv[4], "444") == 0;
  const bool by_budget = std::strcmp(argv[5], "budget") == 0;
  const long n = std::atol(argv[6]);
  auto layout = prores::layout_t::make(w, h, yuv444);
  const int cw = layout.plane_width(1);
  const std::size_t counts[3] = {(std::size_t) w * h, (std::size_t) cw * h, (std::size_t) cw * h};
  std::vector<std::uint16_t> buf(counts[0] + counts[1] + counts[2]);
  const std::uint16_t *planes[3] = {buf.data(), buf.data() + counts[0], buf.data() + counts[0] + counts[1]};
  const std::size_t strides[3] = {(std::size_t) w, (std::size_t) cw, (std::size_t) cw};
  std::vector<std::int16_t> coeffs(layout.total_coeffs);
  std::vector<std::uint8_t> frame;
  prores::quant_t qm;
  prores::color_t color;
  FILE *in = std::fopen(argv[1], "rb"), *out = std::fopen(argv[7], "wb");
  if (!in || !out) {
    std::fprintf(stderr, "cannot open files\n");
    return 1;
  }
  int frames = 0;
  double bytes = 0;
  while (std::fread(buf.data(), 2, buf.size(), in) == buf.size()) {
    prores::forward_dct(layout, planes, strides, coeffs.data());
    int q = (int) n;
    if (by_budget) {
      q = prores::default_ladder[prores::choose_ladder_index(layout, coeffs.data(), (std::size_t) n, prores::default_ladder, qm)];
    }
    prores::write_frame(layout, coeffs.data(), q, color, qm, frame);
    std::fwrite(frame.data(), 1, frame.size(), out);
    bytes += (double) frame.size();
    if (frames++ == 0) {
      std::fprintf(stderr, "q=%d first frame %zu bytes\n", q, frame.size());
    }
  }
  std::fprintf(stderr, "%d frames, average %.0f bytes\n", frames, frames ? bytes / frames : 0.0);
  std::fclose(in);
  std::fclose(out);
  return 0;
}
