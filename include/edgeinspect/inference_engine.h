#pragma once
#include "insplad/detection.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace edgeinspect {
inline constexpr const char* kModelSha256 = "097818af823ab9433e1179449e58d953957a6028b9f2c781f0da6d03fbbc25ec";
struct Config {
    std::filesystem::path root;
    std::uintmax_t max_image_bytes = 32 * 1024 * 1024;
    std::uint64_t max_image_pixels = 24 * 1000 * 1000;
};
struct PreparedInput {
    std::vector<std::uint8_t> rgb; // Owns contiguous RGB NHWC uint8 [1,320,320,3].
    int original_width = 0, original_height = 0;
    double preprocessing_ms = 0;
    std::string method, input_sha256, rgb_sha256;
};
PreparedInput prepare_image(const std::filesystem::path&, const Config&);
PreparedInput prepare_rgb(const std::filesystem::path&);
std::string sha256_file(const std::filesystem::path&);
std::string sha256_bytes(const void*, std::size_t);
std::string json_string(const std::string&);
int asset_class(const std::string&);

struct Diagnostics {
    std::vector<float> predictions, p3, roi, normalized_patches;
};
struct Result {
    std::string asset, bank_sha256;
    insplad::Selection selection;
    double score = 0, model_ms = 0, detection_roi_ms = 0, retrieval_ms = 0, inference_total_ms = 0;
    // No calibrated threshold exists in the original project. Do not invent one.
    std::optional<double> anomaly_threshold;
    std::optional<bool> is_anomaly;
    std::optional<Diagnostics> diagnostics;
};

// One owner, one calling thread. Not copyable/movable; no concurrent infer calls.
// Startup loads one model and all five banks. Results never borrow RKNN memory.
class InferenceEngine {
public:
    explicit InferenceEngine(const Config&);
    ~InferenceEngine(); // noexcept: releases context after all output leases end.
    InferenceEngine(const InferenceEngine&) = delete;
    InferenceEngine& operator=(const InferenceEngine&) = delete;
    Result infer(const PreparedInput&, const std::string& asset, bool diagnostics = false);
    const std::string& environment_json() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::string result_json(const Result&, const PreparedInput&, const std::string& environment);
// Exclusive creation: fail rather than truncate an existing result. Clean partial file on failure.
void save_new_file(const std::filesystem::path&, const void*, std::size_t);
} // namespace edgeinspect
