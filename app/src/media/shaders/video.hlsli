// xbox-cast media: shared shader declarations (included, not compiled on its own).
// One constant buffer for every media shader (VS + PS), filled by VideoRenderer.cpp (struct VideoCb, same layout).
// SPDX-License-Identifier: GPL-3.0-or-later

cbuffer VideoCb : register(b0)
{
    float4 dstRect;    // destination quad in NDC: left, top, right, bottom
    float4 srcRect;    // source rect in texture UV: u0, v0, u1, v1
    float4 cscRow0;    // rgb = M * (s - off), rows of M in xyz
    float4 cscRow1;
    float4 cscRow2;
    float4 cscOff;     // xyz
    float4 lumaSize;   // texture size in texels: w, h, 1/w, 1/h
    float4 aux;        // crop pass: xy = per-output-pixel footprint in UV, z = luma value scale to 8-bit range
    uint4  mode;       // x: 1 = three planes (Y, U, V), 0 = two planes (Y, interleaved UV)
                       // y: 1 = Catmull-Rom luma (upscaling), 0 = bilinear
};

Texture2DArray<float>  texY  : register(t0);
Texture2DArray<float2> texUV : register(t1); // NV12/P010 chroma plane
Texture2DArray<float>  texU  : register(t2); // planar software frames: U (t2) and V (t3)
Texture2DArray<float>  texV  : register(t3);
SamplerState linearClamp : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
