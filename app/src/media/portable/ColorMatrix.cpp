// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "ColorMatrix.h"

namespace XboxCast { namespace Media {

const char* YuvMatrixName(YuvMatrix m)
{
    switch (m) {
    case YuvMatrix::Bt601: return "BT.601";
    case YuvMatrix::Bt709: return "BT.709";
    case YuvMatrix::Bt2020: return "BT.2020";
    }
    return "?";
}

YuvMatrix MatrixFromAvColorspace(int cs, int height)
{
    // AVColorSpace values (libavutil/pixfmt.h): 1 BT709, 5 BT470BG, 6 SMPTE170M, 9 BT2020_NCL, 10 BT2020_CL
    switch (cs) {
    case 1: return YuvMatrix::Bt709;
    case 5:
    case 6: return YuvMatrix::Bt601;
    case 9:
    case 10: return YuvMatrix::Bt2020;
    default: return height > 0 && height < 720 ? YuvMatrix::Bt601 : YuvMatrix::Bt709;
    }
}

CscConstants ComputeCsc(YuvMatrix matrix, bool fullRange, int bits, SampleStorage storage)
{
    // Kr/Kb per standard
    double kr = 0.2126, kb = 0.0722;
    if (matrix == YuvMatrix::Bt601) { kr = 0.299; kb = 0.114; }
    if (matrix == YuvMatrix::Bt2020) { kr = 0.2627; kb = 0.0593; }
    double kg = 1.0 - kr - kb;
    // full-range matrix on normalized (Y in 0..1, Cb/Cr in -0.5..0.5)
    double M[9] = {
        1.0, 0.0, 2.0 * (1.0 - kr),
        1.0, -2.0 * (1.0 - kb) * kb / kg, -2.0 * (1.0 - kr) * kr / kg,
        1.0, 2.0 * (1.0 - kb), 0.0,
    };
    if (bits != 10) bits = 8;
    const double maxCode = (double)((1 << bits) - 1);
    const double sh = (double)(1 << (bits - 8));
    double yMin = fullRange ? 0.0 : 16.0 * sh;
    double yMax = fullRange ? maxCode : 235.0 * sh;
    double cMin = fullRange ? 0.0 : 16.0 * sh;
    double cMax = fullRange ? maxCode : 240.0 * sh;
    double cMid = (double)(1 << (bits - 1));

    // code value per unit sampler value
    double k = 255.0;
    if (storage == SampleStorage::Unorm16Msb) k = 65535.0 / 64.0;   // P010: 10-bit code in the top bits
    else if (storage == SampleStorage::Unorm16Lsb) k = 65535.0;
    if (bits == 8 && storage != SampleStorage::Unorm8) k = (storage == SampleStorage::Unorm16Msb) ? 65535.0 / 256.0 : 65535.0;

    // Y_norm = (code - yMin) / (yMax - yMin); C_norm = (code - cMid) / (cMax - cMin)
    // code = k * s  =>  Y_norm = (k / (yMax-yMin)) * (s - yMin/k)
    double ys = k / (yMax - yMin), cs = k / (cMax - cMin);
    CscConstants c;
    for (int i = 0; i < 3; i++) {
        c.m[i * 3 + 0] = (float)(M[i * 3 + 0] * ys);
        c.m[i * 3 + 1] = (float)(M[i * 3 + 1] * cs);
        c.m[i * 3 + 2] = (float)(M[i * 3 + 2] * cs);
    }
    c.off[0] = (float)(yMin / k);
    c.off[1] = (float)(cMid / k);
    c.off[2] = (float)(cMid / k);
    return c;
}

}} // namespace XboxCast::Media
