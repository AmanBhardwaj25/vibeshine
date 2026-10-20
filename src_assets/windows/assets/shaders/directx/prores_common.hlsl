// Shared by prores_count_cs, prores_choose_cs and prores_write_cs.
//
// The entropy coder (docs/prores-protocol.md section 3.6) exists once, here. Compiled
// without WRITE it only counts bits; compiled with WRITE it emits the same bits, so the
// count and write passes cannot disagree. The coder is ported from FFmpeg's
// proresenc_kostya.c (LGPL 2.1 or later, license compatible with GPL 3) and mirrors
// code_plane() in src/prores/prores_bitstream.cpp.

RWByteAddressBuffer coeffs : register(u0);  // int16 coefficients, two per dword, low half first
RWByteAddressBuffer counts : register(u1);  // bytes per (candidate, slice, plane): index (c * slices + slice) * 3 + plane
RWByteAddressBuffer plan : register(u2);    // per slice uint4 {picture offset, y bytes, cb bytes, cr bytes}; entry [slices] = {picture bytes, q, ladder index, 0}
RWByteAddressBuffer out_buf : register(u3); // [0..15] result copy, [16..] picture

StructuredBuffer<uint4> slice_info : register(t0);  // per slice {mbs, coeff offset y, cb, cr} (offsets in int16 units)

cbuffer prores_params : register(b0) {
    uint slices;
    uint budget_bytes;    // largest picture (picture header + table + slices) that still counts as within budget
    uint capacity_bytes;  // largest picture the output buffer and transport can take
    uint chroma_blocks;   // blocks per macroblock in the chroma planes: 2 (4:2:2) or 4 (4:4:4)
    uint log2_mbs;        // log2 of the macroblocks per slice
    uint3 padding;
    uint4 ladder[4];      // candidate q values, 16 entries
    uint4 qmat[32];       // quantization matrices, 64 bytes each as one uint per byte: luma raster index i at [i], chroma at [64 + i]
};

static const uint scan[64] = {
    0, 1, 8, 9, 2, 3, 10, 11,
    16, 17, 24, 25, 18, 19, 26, 27,
    4, 5, 12, 20, 13, 6, 7, 14,
    21, 28, 29, 22, 15, 23, 30, 31,
    32, 33, 40, 48, 41, 34, 35, 42,
    49, 56, 57, 50, 43, 36, 37, 44,
    51, 58, 59, 52, 45, 38, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};
static const uint dc_cb[7] = {0x04, 0x28, 0x28, 0x4D, 0x4D, 0x70, 0x70};
static const uint run_cb[16] = {0x06, 0x06, 0x05, 0x05, 0x04, 0x29, 0x29, 0x29, 0x29, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x4C};
static const uint level_cb[10] = {0x04, 0x0A, 0x05, 0x06, 0x04, 0x28, 0x28, 0x28, 0x28, 0x4C};

uint ladder_q(uint i) {
    return ladder[i >> 2][i & 3];
}

uint quant_at(uint plane, uint raster) {
    const uint i = (plane ? 64 : 0) + raster;
    return qmat[i >> 2][i & 3];
}

uint bswap(uint x) {
    return (x << 24) | ((x & 0xFF00) << 8) | ((x >> 8) & 0xFF00) | (x >> 24);
}

// ORs the bytes of `be` into the output at byte position `pos`. `be` holds the stream bytes in
// little-endian memory order (bswap of the MSB-first word); bytes that are not part of the
// stream must be zero. Neighbouring writers never share a nonzero byte.
void store_word_be(uint pos, uint be) {
    [branch] if (be == 0) {
        return;
    }
    const uint w = pos & ~3u;
    const uint s = (pos & 3) * 8;
    uint old;
    out_buf.InterlockedOr(w, be << s, old);
    [branch] if (s != 0) {
        const uint hi = be >> (32 - s);
        [branch] if (hi != 0) {
            out_buf.InterlockedOr(w + 4, hi, old);
        }
    }
}

// Bit sink. COUNT: only `bits` is used. WRITE: MSB-first 32-bit accumulator with the
// low `nacc` bits valid and the bits above zero.
struct sink_t {
    uint bits;
    uint acc;
    uint nacc;
    uint pos;  // byte position in out_buf of the next whole word
};

void put(inout sink_t s, uint n, uint val) {
    [branch] if (n == 0) {
        return;
    }
#ifdef WRITE
    [branch] if (n < 32) {
        val &= (1u << n) - 1;
    }
    const uint space = 32 - s.nacc;
    [branch] if (n < space) {
        s.acc = (s.acc << n) | val;
        s.nacc += n;
    } else {
        const uint rem = n - space;  // bits that do not fit
        const uint word = (s.nacc ? (s.acc << space) : 0) | (val >> rem);
        store_word_be(s.pos, bswap(word));
        s.pos += 4;
        s.acc = rem ? (val & ((1u << rem) - 1)) : 0;
        s.nacc = rem;
    }
#else
    s.bits += n;
#endif
}

// Pads the plane to a byte boundary with zero bits.
void flush(inout sink_t s) {
#ifdef WRITE
    [branch] if (s.nacc) {
        store_word_be(s.pos, bswap(s.acc << (32 - s.nacc)));
        s.pos += (s.nacc + 7) >> 3;
        s.acc = 0;
        s.nacc = 0;
    }
#endif
}

void codeword(inout sink_t s, uint cb, uint val) {
    const uint switch_bits = (cb & 3) + 1;
    const uint rice = cb >> 5;
    const uint expo = (cb >> 2) & 7;
    const uint switch_val = switch_bits << rice;
    [branch] if (val >= switch_val) {
        val -= switch_val - (1u << expo);
        const uint e = firstbithigh(val);
        put(s, e - expo + switch_bits, 0);
        put(s, e + 1, val);
    } else {
        const uint e = val >> rice;
        put(s, e, 0);
        put(s, 1, 1);
        [branch] if (rice) {
            put(s, rice, val);
        }
    }
}

int coeff_at(uint idx) {
    const uint w = coeffs.Load((idx >> 1) << 2);
    return ((int) ((idx & 1) ? (w & 0xFFFF0000) : (w << 16))) >> 16;
}

// Division truncating toward zero, spelled out on magnitudes.
int quantize(int v, int d) {
    const int a = abs(v) / d;
    return v < 0 ? -a : a;
}

uint make_code(int x) {
    return (uint) ((x * 2) ^ (x >> 31));
}

// One plane of one slice. `base` is the plane's first coefficient (int16 index), `blocks` the
// blocks in the slice plane.
void code_plane(inout sink_t s, uint base, uint blocks, uint plane, uint q) {
    const int scale = (int) (quant_at(plane, 0) * q);
    int prev = quantize(coeff_at(base) - 0x4000, scale);
    codeword(s, 0xB8, make_code(prev));
    int sign = 0;
    uint cbi = 5;
    for (uint b = 1; b < blocks; b++) {
        const int dc = quantize(coeff_at(base + b) - 0x4000, scale);
        int d = dc - prev;
        const int ns = d >> 31;
        d = (d ^ sign) - sign;
        const uint code = make_code(d);
        codeword(s, dc_cb[cbi], code);
        cbi = min(code, 6u);
        sign = ns;
        prev = dc;
    }
    uint prev_run = 4, prev_level = 2, run = 0;
    for (uint p = 1; p < 64; p++) {
        const int div = (int) (quant_at(plane, scan[p]) * q);
        const uint row = base + p * blocks;
        for (uint b = 0; b < blocks; b++) {
            const int level = quantize(coeff_at(row + b), div);
            [branch] if (level != 0) {
                const uint al = (uint) abs(level);
                codeword(s, run_cb[prev_run], run);
                codeword(s, level_cb[prev_level], al - 1);
                put(s, 1, level < 0 ? 1 : 0);
                prev_run = min(run, 15u);
                prev_level = min(al, 9u);
                run = 0;
            } else {
                run++;
            }
        }
    }
}

uint plane_blocks(uint mbs, uint plane) {
    return mbs * (plane ? chroma_blocks : 4);
}
