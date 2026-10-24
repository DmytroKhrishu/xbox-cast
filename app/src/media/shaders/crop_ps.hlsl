// xbox-cast media: crop-detection downsample. Renders the luma plane into a small R8 target (256x144); every output
// pixel averages a 4x4 grid of bilinear taps (each tap already averages 2x2 texels) across its footprint, which is
// close to a box filter for the ~7.5x reduction of 1080p. The CPU reads the result back asynchronously.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video.hlsli"

float4 main(VsOut i) : SV_Target
{
    float sum = 0.0;
    [unroll] for (int y = 0; y < 4; y++) {
        [unroll] for (int x = 0; x < 4; x++) {
            float2 o = (float2(x, y) - 1.5) * 0.25 * aux.xy;
            sum += texY.SampleLevel(linearClamp, float3(i.uv + o, 0), 0);
        }
    }
    return float4(saturate(sum * (1.0 / 16.0) * aux.z), 0, 0, 1);
}
