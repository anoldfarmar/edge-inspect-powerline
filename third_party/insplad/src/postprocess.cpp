#include "insplad/postprocess.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace insplad {
namespace {

void validate_box(const InputBox& box) {
    for (float v : box) {
        if (!std::isfinite(v)) throw std::invalid_argument("Non-finite input box");
    }
    if (!(box[2] > box[0] && box[3] > box[1])) {
        throw std::invalid_argument("Box must have positive width and height");
    }
}

void validate_tensor(const float* data, std::size_t elements) {
    if (!data || elements != kFeatureElements) {
        throw std::invalid_argument("Expected 102400 FP32 elements");
    }
    for (std::size_t i = 0; i < elements; ++i) {
        if (!std::isfinite(data[i])) throw std::invalid_argument("Non-finite tensor");
    }
}

SamplingPoint sample(const InputBox& box, int y, int x) {
    // Apply scale and aligned offset once, following the FP32 RoIAlign order.
    constexpr float scale = 0.125f;
    const float left = box[0] * scale - 0.5f;
    const float top = box[1] * scale - 0.5f;
    const float right = box[2] * scale - 0.5f;
    const float bottom = box[3] * scale - 0.5f;
    const float bin_w = (right - left) / kRoiWidth;
    const float bin_h = (bottom - top) / kRoiHeight;
    SamplingPoint s{};
    s.x = left + x * bin_w + 0.5f * bin_w;
    s.y = top + y * bin_h + 0.5f * bin_h;
    if (!std::isfinite(s.x) || !std::isfinite(s.y)) {
        throw std::invalid_argument("Box produces non-finite sample coordinates");
    }
    // Strict inequalities: exactly -1 and exactly 40 still sample the border.
    s.outside = s.x < -1.0f || s.x > kP3Width || s.y < -1.0f || s.y > kP3Height;
    if (s.outside) {
        s.xs.fill(-1);
        s.ys.fill(-1);
        return s;
    }
    const float cx = std::max(0.0f, std::min(s.x, float(kP3Width - 1)));
    const float cy = std::max(0.0f, std::min(s.y, float(kP3Height - 1)));
    const int xl = static_cast<int>(cx);
    const int yl = static_cast<int>(cy);
    const int xh = std::min(xl + 1, kP3Width - 1);
    const int yh = std::min(yl + 1, kP3Height - 1);
    const float lx = cx - xl;
    const float ly = cy - yl;
    const float hx = 1.0f - lx;
    const float hy = 1.0f - ly;
    s.xs = {{xl, xh, xl, xh}};
    s.ys = {{yl, yl, yh, yh}};
    s.weights = {{hy * hx, hy * lx, ly * hx, ly * lx}};
    return s;
}

}  // namespace

SamplingPoint roi_sampling_point(const InputBox& box, int roi_y, int roi_x) {
    validate_box(box);
    if (roi_y < 0 || roi_y >= kRoiHeight || roi_x < 0 || roi_x >= kRoiWidth) {
        throw std::out_of_range("ROI sample index");
    }
    return sample(box, roi_y, roi_x);
}

std::vector<float> roi_align_p3(const float* p3, std::size_t elements,
                              const InputBox& box) {
    validate_tensor(p3, elements);
    validate_box(box);
    std::vector<float> roi(kFeatureElements, 0.0f);
    for (int y = 0; y < kRoiHeight; ++y) {
        for (int x = 0; x < kRoiWidth; ++x) {
            const SamplingPoint s = sample(box, y, x);
            if (s.outside) continue;
            for (int c = 0; c < kP3Channels; ++c) {
                const float* channel = p3 + c * kP3Height * kP3Width;
                const float v1 = channel[s.ys[0] * kP3Width + s.xs[0]];
                const float v2 = channel[s.ys[1] * kP3Width + s.xs[1]];
                const float v3 = channel[s.ys[2] * kP3Width + s.xs[2]];
                const float v4 = channel[s.ys[3] * kP3Width + s.xs[3]];
                roi[c * kPatches + y * kRoiWidth + x] =
                    s.weights[0] * v1 + s.weights[1] * v2 +
                    s.weights[2] * v3 + s.weights[3] * v4;
            }
        }
    }
    return roi;
}

std::vector<float> normalize_roi_patches(const float* roi, std::size_t elements) {
    validate_tensor(roi, elements);
    std::vector<float> normalized(kFeatureElements);
    for (int p = 0; p < kPatches; ++p) {
        // Accumulate in double to avoid FP32 square overflow/underflow.
        double squared_norm = 0.0;
        for (int c = 0; c < kP3Channels; ++c) {
            const double v = roi[c * kPatches + p];
            squared_norm += v * v;
        }
        const double norm = std::max(std::sqrt(squared_norm), 1e-12);
        for (int c = 0; c < kP3Channels; ++c) {
            normalized[p * kP3Channels + c] = static_cast<float>(roi[c * kPatches + p] / norm);
        }
    }
    return normalized;
}

P3PostprocessResult postprocess_p3(const float* p3, std::size_t elements,
                                 const InputBox& box) {
    P3PostprocessResult result;
    result.roi = roi_align_p3(p3, elements, box);
    result.normalized_patches = normalize_roi_patches(result.roi.data(), result.roi.size());
    return result;
}

}  // namespace insplad
