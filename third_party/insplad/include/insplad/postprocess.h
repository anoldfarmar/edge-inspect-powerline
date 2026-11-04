#ifndef INSPLAD_POSTPROCESS_H
#define INSPLAD_POSTPROCESS_H

#include <array>
#include <cstddef>
#include <vector>

namespace insplad {

constexpr int kP3Channels = 64;
constexpr int kP3Height = 40;
constexpr int kP3Width = 40;
constexpr int kRoiHeight = 40;
constexpr int kRoiWidth = 40;
constexpr int kPatches = kRoiHeight * kRoiWidth;
constexpr std::size_t kFeatureElements = kP3Channels * kPatches;

// Continuous xyxy in 320x320 MODEL INPUT pixels. Do not pre-scale or clip.
using InputBox = std::array<float, 4>;

struct SamplingPoint {
    float x;
    float y;
    bool outside;
    // Neighbors ordered (y_low,x_low), (y_low,x_high),
    // (y_high,x_low), (y_high,x_high). -1 for outside samples.
    std::array<int, 4> xs;
    std::array<int, 4> ys;
    std::array<float, 4> weights;
};

// The same sampling routine used by roi_align_p3; exposed for diagnostics.
SamplingPoint roi_sampling_point(const InputBox& box, int roi_y, int roi_x);

// Raw FP32 contiguous NCHW [1,64,40,40] -> contiguous [64,40,40].
// Fixed: size=40x40, spatial_scale=.125, aligned=true, sampling_ratio=1.
// Rejects non-finite data and non-positive box extents; permits out-of-bounds boxes.
std::vector<float> roi_align_p3(const float* p3, std::size_t elements,
                              const InputBox& box);

// ROI [64,40,40] -> patches [1600,64], q[p,c] = r[c,p]/max(||r[:,p]||2,1e-12).
std::vector<float> normalize_roi_patches(const float* roi, std::size_t elements);

struct P3PostprocessResult {
    std::vector<float> roi;
    std::vector<float> normalized_patches;
};

// Deployment entry point: raw P3 -> RoIAlign -> patch channel-L2 normalization.
P3PostprocessResult postprocess_p3(const float* p3, std::size_t elements,
                                 const InputBox& box);

}  // namespace insplad
#endif
