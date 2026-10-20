// Exact bytes of every (candidate q, slice, plane): runs the entropy coder of
// prores_common.hlsl in count mode. Dispatch(ceil(slices / 64), 3, 16).

#include "prores_common.hlsl"

[numthreads(64, 1, 1)]
void main_cs(uint3 id : SV_DispatchThreadID) {
    const uint slice = id.x, plane = id.y, cand = id.z;
    if (slice >= slices) {
        return;
    }
    const uint4 info = slice_info[slice];
    sink_t s = (sink_t) 0;
    code_plane(s, info[1 + plane], plane_blocks(info.x, plane), plane, ladder_q(cand));
    counts.Store(((cand * slices + slice) * 3 + plane) * 4, (s.bits + 7) >> 3);
}
