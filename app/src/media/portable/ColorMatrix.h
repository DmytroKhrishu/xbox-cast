// xbox-cast media: YUV -> RGB conversion constants (portable).
// Matrix coefficients follow moonlight-xbox / moonlight-qt (GPL-3.0) VideoRenderer::getFramePremultipliedCscConstants,
// generalized so the shader can work on raw sampler values of any storage format (8-bit UNORM, P010 MSB-aligned
// 16-bit UNORM, LSB-aligned 10-bit in R16_UNORM): rgb = M' * (s - off'), s = sampler value in [0,1].
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace XboxCast { namespace Media {

enum class YuvMatrix { Bt601, Bt709, Bt2020 };

enum class SampleStorage {
    Unorm8,          // R8/R8G8 UNORM: s = code / 255
    Unorm16Msb,      // P010/P016 in R16 UNORM: code10 = s * 65535 / 64
    Unorm16Lsb,      // yuv420p10 uploaded to R16 UNORM: code10 = s * 65535
};

struct CscConstants {
    float m[9];      // row-major: rgb[i] = sum_j m[i*3+j] * (s[j] - off[j]), s = (Y, Cb, Cr)
    float off[3];
};

// bits: 8 or 10. fullRange: JPEG/full range.
CscConstants ComputeCsc(YuvMatrix matrix, bool fullRange, int bits, SampleStorage storage);

const char* YuvMatrixName(YuvMatrix m);

// Map an FFmpeg AVColorSpace value (int to stay header-free) to a matrix. Unknown/unspecified: by height (SD = 601,
// otherwise 709), which is what iOS sends in practice (BT.709 VUI).
YuvMatrix MatrixFromAvColorspace(int avColorspace, int height);

}} // namespace XboxCast::Media
