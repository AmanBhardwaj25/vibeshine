# ChloroFrame ProRes extension (wire contract and bitstream)

This is the ChloroFrame ProRes extension to the Sunshine/Vibeshine video protocol. It is copied verbatim from sections 2.3 and 3 of the ChloroFrame ProRes plan. Host and client must match it exactly.

### 2.3 Wire contract (host and client must match exactly)

| Item | Value |
|---|---|
| `SCM_PRORES_422` | `0x08000000` (10-bit 4:2:2; HDR10 when the host display is HDR) |
| `SCM_PRORES_444` | `0x10000000` (10-bit 4:4:4; HDR10 when the host display is HDR) |
| `x-nv-vqos[0].bitStreamFormat` | `4` |
| `x-ss-video[0].chromaSamplingType` | `0` = 4:2:2, `1` = 4:4:4 (reused field) |
| `x-nv-video[0].encoderCscMode` | `2` (BT.709 limited) for SDR, `4` (BT.2020 limited, PQ) for HDR, same as PyroWave |
| `x-nv-video[0].dynamicRangeMode` | `1` requests HDR10; host may fall back to SDR (colorimetry is in-band anyway) |
| RTSP DESCRIBE line | `a=x-cf-prores:1` (protocol version 1). The client refuses ProRes and falls back to HEVC if it is missing or not `1`. |
| RTP payload | One complete ProRes frame per video frame (section 3), inside the normal Sunshine video packets. |
| Short frame header bytes 6 and 7 | critical packet count (`pyrowave_critical_packets`), set from `critical_bytes`. |
| IDR / reference invalidation requests | ignored by the host (every frame is intra), same as PyroWave. |

## 3. ProRes bitstream reference (what we produce, byte exact)

Big-endian throughout. This is everything the encoder and the client parser need.
Cross-check against `referenceonly/ffmpeg/libavcodec/proresenc_kostya.c` (`encode_frame`,
`encode_slice`, `encode_dcs`, `encode_acs`, `encode_vlc_codeword`) and `proresdec.c`.

### 3.1 Frame

| Offset | Size | Value |
|---|---|---|
| 0 | 4 | frame size in bytes, including these 8 bytes |
| 4 | 4 | `icpf` |
| 8 | 2 | frame header size = 20 (148 if quant matrices are sent) |
| 10 | 2 | bitstream version: 0 for 4:2:2, 1 for 4:4:4 |
| 12 | 4 | vendor: `cfrm` |
| 16 | 2 | width (pixels) |
| 18 | 2 | height (pixels) |
| 20 | 1 | `chroma << 6`: `0x80` = 4:2:2, `0xC0` = 4:4:4 (progressive, bits 3..2 = 0) |
| 21 | 1 | 0 (aspect ratio and frame rate codes unknown) |
| 22 | 1 | color primaries: 1 = BT.709, 9 = BT.2020 |
| 23 | 1 | transfer: 1 = BT.709, 16 = SMPTE ST 2084 (PQ) |
| 24 | 1 | matrix: 1 = BT.709, 9 = BT.2020 non-constant |
| 25 | 1 | alpha info: 0 |
| 26 | 1 | 0 |
| 27 | 1 | matrix flags: 0 = default (flat 4s), 0x03 = luma matrix then chroma matrix follow (64 bytes each, raster order) |
| 28 | 8 | picture header |
| 36 | 2 x slices | slice table: BE16 size of each slice, in slice order |
| ... | | slices |

### 3.2 Picture header (8 bytes)

`[0] = 0x40` (8 bytes << 3), `[1..4]` = picture size in bytes (picture header + table + all slices),
`[5..6]` = slice count, `[7]` = `log2(mbs_per_slice) << 4` (slice height is always 1 MB).

### 3.3 Slice (3 planes, no alpha)

`[0] = 0x30` (6-byte header << 3), `[1]` = q (1..128 here), `[2..3]` = Y data bytes,
`[4..5]` = Cb data bytes, then Y data, Cb data, Cr data (Cr size is implied).
Each plane's data is padded with zero bits to a byte boundary. A slice must be smaller than 65536 bytes
(it always is: 8 MBs at q=1 in 4:4:4 is bounded far below that).

Decoder q mapping (`proresdec.c:654`): byte values 1..128 are the scale itself. We never write > 128.
Effective quantizer per coefficient: `qmat[i] * q` (raster index `i`), separate luma and chroma matrices.

### 3.4 Samples and DCT

- Samples are 10-bit video-range code values (Y 64..940, C 64..960 nominal; clamp to 0..1023).
- 2D orthonormal DCT-II on each 8x8 block (`dct.glsl:fdct8`: rows then columns, `c_norm = 1/sqrt(8)`).
- Coefficient = `trunc(4 * dct_value)` as int (fits int16). Check: a flat mid-gray block (512) gives DC = `4 * 8 * 512 = 16384 = 0x4000`.
- Reference: the FFmpeg Vulkan encoder normalizes samples by `2^(bits-1)` and scales by `2^11`, which is the same `4x` for 10-bit input.

### 3.5 Macroblocks, blocks, slices, coefficient order

- Macroblock = 16x16 luma. Blocks per MB: luma 4 (order **TL, TR, BL, BR**); chroma 4:2:2: 2 per plane
  (an 8x16 chroma MB: order **top, bottom**); chroma 4:4:4: 4 per plane (order **TL, BL, TR, BR**:
  note this differs from luma; see `prores_ks_slice_data.comp.glsl` `block_coord`).
- Pixels outside the picture: clamp the fetch coordinate to the last row or column (edge replication).
- Slices: one MB tall. Per MB row, `floor(mb_width / 8)` slices of 8 MBs, then the remainder split into
  descending powers of two (remainder 5 becomes 4 then 1). Slices are ordered row by row, left to right.
  Slice count = `mb_height * (floor(mb_width/8) + popcount(mb_width % 8))`. Example: 3024x1890 is
  189 x 119 MBs, 25 slices per row, 2975 slices.
- Coefficient storage and coding order inside a slice plane (`blocks_per_slice = mbs * blocks_per_mb`):
  DC: block 0, 1, 2 ... in block order. AC: for scan position `p = 1..63`, for block `b = 0..blocks_per_slice-1`:
  coefficient `raster = progressive_scan[p]` of block `b`. Storage index in `coeffs` =
  `coeff_offset[plane] + p * blocks_per_slice + b` (so the coder reads sequentially).
- `progressive_scan` (raster indices): `0,1,8,9,2,3,10,11, 16,17,24,25,18,19,26,27, 4,5,12,20,13,6,7,14, 21,28,29,22,15,23,30,31, 32,33,40,48,41,34,35,42, 49,56,57,50,43,36,37,44, 51,58,59,52,45,38,39,46, 53,60,61,54,47,55,62,63` (`proresdata.c`).

### 3.6 Entropy coding (per slice, per plane)

```
codeword(cb, val):                       // cb is a codebook byte, val >= 0
  switch_bits = (cb & 3) + 1; rice = cb >> 5; expo = (cb >> 2) & 7
  switch_val = switch_bits << rice
  if val >= switch_val:
     val -= switch_val - (1 << expo); e = floor(log2(val))
     put(e - expo + switch_bits zero bits); put(e + 1 bits of val)
  else:
     e = val >> rice; put(e zero bits); put(1); if rice: put(rice low bits of val)
  bit count = (val >= switch_val) ? 2*e - expo + switch_bits + 1 : (val >> rice) + rice + 1

MAKE_CODE(x) = (x * 2) ^ (x >> 31)        // sign folding
DC: scale = qmat[0] * q
  prev = (c[0] - 0x4000) / scale; codeword(0xB8, MAKE_CODE(prev)); sign = 0; cbi = 5
  for each next block: dc = (c - 0x4000) / scale; d = dc - prev; ns = d >> 31
     d = (d ^ sign) - sign; code = MAKE_CODE(d); codeword(DC_CB[cbi], code)
     cbi = min(code, 6); sign = ns; prev = dc
AC: prev_run = 4; prev_level = 2; run = 0
  for p in 1..63: for b in blocks: level = c / (qmat[scan[p]] * q)
     if level != 0: codeword(RUN_CB[prev_run], run); codeword(LEVEL_CB[prev_level], |level| - 1)
                    put(1 bit: 1 if level < 0); prev_run = min(run, 15); prev_level = min(|level|, 9); run = 0
     else: run++
  (trailing zeros are not coded)
DC_CB    = 04 28 28 4D 4D 70 70
RUN_CB   = 06 06 05 05 04 29 29 29 29 28 28 28 28 28 28 4C
LEVEL_CB = 04 0A 05 06 04 28 28 28 28 4C
```

The count pass must produce exactly `ceil(bits / 8)` for what the write pass emits. Share one HLSL function with a `WRITE` switch.
