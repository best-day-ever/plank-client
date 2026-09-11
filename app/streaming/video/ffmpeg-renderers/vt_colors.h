#pragma once
#include <simd/simd.h>
#include <cstddef>

// Shared with vt_renderer.metal. float3 occupies a 16-byte aligned slot.
struct PlankVTColorParams {
    simd_float3 matrix[3];
    simd_float3 offsets;
    float bitnessScaleFactor;
};
static_assert(offsetof(PlankVTColorParams, offsets) == 48);
static_assert(offsetof(PlankVTColorParams, bitnessScaleFactor) == 64);
static_assert(sizeof(PlankVTColorParams) == 80);

enum class PlankVTMatrix { IdentityGbr, Bt601, Bt709, Bt2020 };

inline PlankVTColorParams plankVTColorParams(PlankVTMatrix matrix, bool fullRange,
                                            int depth, bool highBits)
{
    PlankVTColorParams params{};
    const float maximum = float((1 << depth) - 1);
    const float shift = float(1 << (depth - 8));
    params.bitnessScaleFactor = depth == 8 ? 1.0f :
            65535.0f / (maximum * (highBits ? float(1 << (16 - depth)) : 1.0f));
    if (matrix == PlankVTMatrix::IdentityGbr) {
        params.matrix[0] = {0, 0, 1};
        params.matrix[1] = {1, 0, 0};
        params.matrix[2] = {0, 1, 0};
        return params;
    }
    const float kr = matrix == PlankVTMatrix::Bt601 ? 0.299f :
                     matrix == PlankVTMatrix::Bt2020 ? 0.2627f : 0.2126f;
    const float kb = matrix == PlankVTMatrix::Bt601 ? 0.114f :
                     matrix == PlankVTMatrix::Bt2020 ? 0.0593f : 0.0722f;
    const float kg = 1 - kr - kb;
    const float ys = fullRange ? 1 : maximum / (219 * shift);
    const float cs = fullRange ? 1 : maximum / (224 * shift);
    params.matrix[0] = {ys, 0, 2 * (1-kr) * cs};
    params.matrix[1] = {ys, -2 * kb * (1-kb) / kg * cs, -2 * kr * (1-kr) / kg * cs};
    params.matrix[2] = {ys, 2 * (1-kb) * cs, 0};
    params.offsets = {fullRange ? 0 : 16 * shift / maximum,
                      128 * shift / maximum, 128 * shift / maximum};
    return params;
}
