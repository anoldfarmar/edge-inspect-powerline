#include "insplad/postprocess.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Adapter only: binary IO and diagnostics, all numerical work in the deployment library.
static std::vector<float> read_f32(const std::string& path, std::size_t count) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != static_cast<std::streamoff>(count * 4)) {
        throw std::runtime_error("Wrong input byte count: " + path);
    }
    file.seekg(0);
    std::vector<float> values(count);
    if (!file.read(reinterpret_cast<char*>(values.data()), count * 4)) {
        throw std::runtime_error("Read failed: " + path);
    }
    return values;
}

static void write_f32(const std::string& path, const std::vector<float>& values) {
    // Refuse to replace existing outputs.
    if (std::ifstream(path, std::ios::binary).good()) throw std::runtime_error("Output exists: " + path);
    for (float v : values) {
        if (!std::isfinite(v)) throw std::runtime_error("Non-finite computed output: " + path);
    }
    std::ofstream file(path, std::ios::binary);
    if (!file.write(reinterpret_cast<const char*>(values.data()), values.size() * 4)) {
        throw std::runtime_error("Write failed: " + path);
    }
}

int main(int argc, char** argv) {
    try {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE754 FP32 required");
        const std::uint32_t endian = 1;
        if (*reinterpret_cast<const unsigned char*>(&endian) != 1) throw std::runtime_error("Little-endian host required");
        const bool trace = argc == 8 && std::string(argv[1]) == "--trace";
        if (argc != 5 && !trace) {
            std::cerr << "Usage: insplad_roi_fixture P3.f32 BOX.f32 ROI.f32 NORMALIZED.f32\n"
                         "   or: insplad_roi_fixture --trace P3.f32 BOX.f32 CHANNEL Y X TRACE.json\n";
            return 1;
        }
        const int first = trace ? 2 : 1;
        const auto p3 = read_f32(argv[first], insplad::kFeatureElements);
        const auto box_values = read_f32(argv[first + 1], 4);
        insplad::InputBox box{};
        std::copy(box_values.begin(), box_values.end(), box.begin());
        const auto result = insplad::postprocess_p3(p3.data(), p3.size(), box);
        if (!trace) {
            write_f32(argv[3], result.roi);
            write_f32(argv[4], result.normalized_patches);
            std::cout << "Computed ROI and normalized patches via insplad::postprocess_p3\n";
            return 0;
        }
        const int c = std::stoi(argv[4]);
        const int y = std::stoi(argv[5]);
        const int x = std::stoi(argv[6]);
        if (c < 0 || c >= insplad::kP3Channels) throw std::out_of_range("Channel");
        const auto s = insplad::roi_sampling_point(box, y, x);
        const int patch = y * insplad::kRoiWidth + x;
        double squared_norm = 0;
        for (int ch = 0; ch < insplad::kP3Channels; ++ch) {
            const double v = result.roi[ch * insplad::kPatches + patch];
            squared_norm += v * v;
        }
        std::ofstream f(argv[7]);
        f.exceptions(std::ios::badbit | std::ios::failbit);
        f << std::setprecision(17) << "{\"c\":" << c << ",\"y\":" << y << ",\"x\":" << x
          << ",\"sample_x\":" << s.x << ",\"sample_y\":" << s.y
          << ",\"outside\":" << (s.outside ? "true" : "false") << ",\"neighbors\":[";
        for (int i = 0; i < 4; ++i) {
            if (i) f << ',';
            f << "{\"x\":" << s.xs[i] << ",\"y\":" << s.ys[i] << ",\"weight\":" << s.weights[i] << ",\"p3_value\":";
            if (s.outside) f << "null";
            else f << p3[c * insplad::kPatches + s.ys[i] * insplad::kP3Width + s.xs[i]];
            f << '}';
        }
        f << "],\"roi_value\":" << result.roi[c * insplad::kPatches + patch]
          << ",\"patch_l2_denominator\":" << std::max(std::sqrt(squared_norm), 1e-12)
          << ",\"normalized_value\":" << result.normalized_patches[patch * insplad::kP3Channels + c] << "}\n";
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 2;
    }
    return 0;
}
