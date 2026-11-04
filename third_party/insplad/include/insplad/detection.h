#ifndef INSPLAD_DETECTION_H
#define INSPLAD_DETECTION_H
#include "insplad/postprocess.h"
#include <vector>
namespace insplad {
struct Detection {
    InputBox xyxy;
    float confidence;
    int class_id;
    int candidate;
};
struct Selection {
    InputBox box = {{0, 0, 320, 320}};
    float confidence = 0;
    bool fallback = true;
    int candidates = 0;
};
// Decoded BCN [1,18,2100]; conf=.25, class-aware IoU=.7, max_det=300.
std::vector<Detection> decode_nms(const float* predictions, std::size_t elements);
Selection select_asset(const std::vector<Detection>& detections, int expected_class);
}
#endif
