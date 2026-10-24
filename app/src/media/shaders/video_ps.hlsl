// xbox-cast media: YUV (NV12 / P010 / planar 8-10 bit) -> RGB with the stream's matrix and range.
// Luma: Catmull-Rom bicubic when upscaling (9 bilinear taps instead of 16 point taps, the standard optimization by
// J. Jimenez / "Filmic SMAA"), bilinear otherwise. Chroma: bilinear (it is half resolution and soft anyway).
// Cost on Series S at 1080p: 10 texture fetches per pixel.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video.hlsli"

float SampleLumaCatmullRom(float2 uv)
{
    float2 samplePos = uv * lumaSize.xy;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;

    float2 texPos0 = (texPos1 - 1.0) * lumaSize.zw;
    float2 texPos3 = (texPos1 + 2.0) * lumaSize.zw;
    float2 texPos12 = (texPos1 + offset12) * lumaSize.zw;

    float r = 0.0;
    r += texY.SampleLevel(linearClamp, float3(texPos0.x,  texPos0.y,  0), 0) * w0.x  * w0.y;
    r += texY.SampleLevel(linearClamp, float3(texPos12.x, texPos0.y,  0), 0) * w12.x * w0.y;
    r += texY.SampleLevel(linearClamp, float3(texPos3.x,  texPos0.y,  0), 0) * w3.x  * w0.y;
    r += texY.SampleLevel(linearClamp, float3(texPos0.x,  texPos12.y, 0), 0) * w0.x  * w12.y;
    r += texY.SampleLevel(linearClamp, float3(texPos12.x, texPos12.y, 0), 0) * w12.x * w12.y;
    r += texY.SampleLevel(linearClamp, float3(texPos3.x,  texPos12.y, 0), 0) * w3.x  * w12.y;
    r += texY.SampleLevel(linearClamp, float3(texPos0.x,  texPos3.y,  0), 0) * w0.x  * w3.y;
    r += texY.SampleLevel(linearClamp, float3(texPos12.x, texPos3.y,  0), 0) * w12.x * w3.y;
    r += texY.SampleLevel(linearClamp, float3(texPos3.x,  texPos3.y,  0), 0) * w3.x  * w3.y;
    return r;
}

float4 main(VsOut i) : SV_Target
{
    // if, not ?: (HLSL evaluates both operands of ?:, which would run the 9 taps on every pixel)
    float y;
    [branch] if (mode.y != 0)
        y = SampleLumaCatmullRom(i.uv);
    else
        y = texY.SampleLevel(linearClamp, float3(i.uv, 0), 0);
    float2 c;
    if (mode.x != 0)
        c = float2(texU.SampleLevel(linearClamp, float3(i.uv, 0), 0), texV.SampleLevel(linearClamp, float3(i.uv, 0), 0));
    else
        c = texUV.SampleLevel(linearClamp, float3(i.uv, 0), 0);
    float3 s = float3(y, c) - cscOff.xyz;
    float3 rgb = float3(dot(cscRow0.xyz, s), dot(cscRow1.xyz, s), dot(cscRow2.xyz, s));
    return float4(saturate(rgb), 1.0);
}
