// Emits the slice data: the entropy coder of prores_common.hlsl in write mode, one thread
// per (slice, plane). Reads q and the byte offsets planned by prores_choose_cs.
// Dispatch(ceil(slices / 64), 3, 1). out_buf must be zeroed: bits are ORed in.

#define WRITE
#include "prores_common.hlsl"

[numthreads(64, 1, 1)]
void main_cs(uint3 id : SV_DispatchThreadID) {
    const uint slice = id.x, plane = id.y;
    if (slice >= slices) {
        return;
    }
    const uint4 result = plan.Load4(slices * 16);
    if (result.x == 0) {
        return;  // frame skipped
    }
    const uint4 entry = plan.Load4(slice * 16);
    const uint4 info = slice_info[slice];
    sink_t s = (sink_t) 0;
    s.pos = 16 + entry.x + 6 + (plane >= 1 ? entry.y : 0) + (plane >= 2 ? entry.z : 0);
    code_plane(s, info[1 + plane], plane_blocks(info.x, plane), plane, result.y);
    flush(s);
}
