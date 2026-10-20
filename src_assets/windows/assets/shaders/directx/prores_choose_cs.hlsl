// Picks the finest q whose picture fits the budget, plans the slice offsets and writes the
// picture header, slice table and slice headers. One group, Dispatch(1, 1, 1).
//
// total[c] = 8 + 2 * slices + sum over slices (6 + y + cb + cr), from the count pass.
// Lowest c (best quality) with total <= budget; c = 15 when none fits; the frame is skipped
// (picture bytes = 0) when even that exceeds the capacity.

#include "prores_common.hlsl"

#define GROUP 1024

groupshared uint g_total[16];
groupshared uint g_scan[GROUP];

uint slice_total(uint cand, uint slice) {
    const uint3 n = counts.Load3(((cand * slices + slice) * 3) * 4);
    return 6 + n.x + n.y + n.z;
}

[numthreads(GROUP, 1, 1)]
void main_cs(uint t : SV_GroupIndex) {
    if (t < 16) {
        g_total[t] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint c = 0; c < 16; c++) {
        uint sum = 0;
        for (uint s = t; s < slices; s += GROUP) {
            sum += slice_total(c, s);
        }
        InterlockedAdd(g_total[c], sum);
    }
    GroupMemoryBarrierWithGroupSync();

    // Every thread computes the same pick from the same totals.
    const uint fixed_bytes = 8 + 2 * slices;
    uint pick = 15;
    [unroll]
    for (int i = 14; i >= 0; i--) {
        if (g_total[i] + fixed_bytes <= budget_bytes) {
            pick = i;
        }
    }
    const uint picture = g_total[pick] + fixed_bytes;
    const bool skip = picture > capacity_bytes;
    const uint q = ladder_q(pick);

    // Exclusive prefix sum of the slice sizes: contiguous chunk per thread, then a scan of the chunk sums.
    const uint chunk = (slices + GROUP - 1) / GROUP;
    const uint first = min(t * chunk, slices);
    const uint last = min(first + chunk, slices);
    uint mine = 0;
    for (uint k = first; k < last; k++) {
        mine += slice_total(pick, k);
    }
    g_scan[t] = mine;
    GroupMemoryBarrierWithGroupSync();
    for (uint o = 1; o < GROUP; o <<= 1) {
        const uint v = t >= o ? g_scan[t - o] : 0;
        GroupMemoryBarrierWithGroupSync();
        g_scan[t] += v;
        GroupMemoryBarrierWithGroupSync();
    }

    if (skip) {
        if (t == 0) {
            plan.Store4(slices * 16, uint4(0, 0, 0, 0));
            out_buf.Store4(0, uint4(0, 0, 0, 0));
        }
        return;
    }

    uint offset = fixed_bytes + g_scan[t] - mine;
    for (uint k2 = first; k2 < last; k2++) {
        const uint3 n = counts.Load3(((pick * slices + k2) * 3) * 4);
        const uint size = 6 + n.x + n.y + n.z;
        plan.Store4(k2 * 16, uint4(offset, n.x, n.y, n.z));
        // slice header: 0x30, q, y bytes (BE16), cb bytes (BE16)
        store_word_be(16 + offset, bswap((0x30u << 24) | (q << 16) | n.x));
        store_word_be(16 + offset + 4, bswap(n.y << 16));
        // slice table entry (BE16)
        store_word_be(16 + 8 + 2 * k2, bswap(size << 16));
        offset += size;
    }

    if (t == 0) {
        // picture header: 0x40, picture size (BE32), slice count (BE16), log2(mbs) << 4
        store_word_be(16, bswap((0x40u << 24) | (picture >> 8)));
        store_word_be(20, bswap(((picture & 0xFF) << 24) | (slices << 8) | (log2_mbs << 4)));
        const uint4 result = uint4(picture, q, pick, 0);
        plan.Store4(slices * 16, result);
        out_buf.Store4(0, result);
    }
}
