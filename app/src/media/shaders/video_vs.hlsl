// xbox-cast media: one textured quad, generated from SV_VertexID (draw 4 vertices as a triangle strip, no buffers).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video.hlsli"

VsOut main(uint id : SV_VertexID)
{
    // 0: left-top, 1: right-top, 2: left-bottom, 3: right-bottom
    float2 t = float2(id & 1, id >> 1);
    VsOut o;
    o.pos = float4(lerp(dstRect.x, dstRect.z, t.x), lerp(dstRect.y, dstRect.w, t.y), 0.0, 1.0);
    o.uv = float2(lerp(srcRect.x, srcRect.z, t.x), lerp(srcRect.y, srcRect.w, t.y));
    return o;
}
