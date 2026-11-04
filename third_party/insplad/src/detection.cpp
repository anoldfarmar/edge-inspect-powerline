#include "insplad/detection.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace insplad {
namespace {
bool ranked(const Detection& a, const Detection& b) {
    if (a.confidence != b.confidence) return a.confidence > b.confidence;
    if (a.xyxy != b.xyxy) return a.xyxy < b.xyxy;
    return a.candidate < b.candidate;
}
double iou(const InputBox& a, const InputBox& b) {
    const double w = std::max(0.0, double(std::min(a[2], b[2])) - std::max(a[0], b[0]));
    const double h = std::max(0.0, double(std::min(a[3], b[3])) - std::max(a[1], b[1]));
    const double aa = std::max(0.0, double(a[2])-a[0]) * std::max(0.0, double(a[3])-a[1]);
    const double ab = std::max(0.0, double(b[2])-b[0]) * std::max(0.0, double(b[3])-b[1]);
    return w*h / std::max(aa+ab-w*h, 1e-12);
}
}
std::vector<Detection> decode_nms(const float* pred, std::size_t elements) {
    if (!pred || elements != 18*2100) throw std::invalid_argument("Expected predictions [1,18,2100]");
    for (std::size_t i=0; i<elements; ++i) {
        if (!std::isfinite(pred[i])) throw std::invalid_argument("Non-finite prediction");
        if (i>=4*2100 && (pred[i]<0 || pred[i]>1)) throw std::invalid_argument("Invalid class probability");
    }
    std::vector<Detection> candidates, kept;
    for (int i=0; i<2100; ++i) {
        int cls=0;
        for (int c=1; c<14; ++c) if (pred[(4+c)*2100+i]>pred[(4+cls)*2100+i]) cls=c;
        const float score=pred[(4+cls)*2100+i];
        if (score<=.25f) continue;
        const float cx=pred[i], cy=pred[2100+i], w=pred[4200+i], h=pred[6300+i];
        Detection d;
        d.xyxy={{cx-w*.5f,cy-h*.5f,cx+w*.5f,cy+h*.5f}};
        d.confidence=score; d.class_id=cls; d.candidate=i;
        candidates.push_back(d);
    }
    std::stable_sort(candidates.begin(), candidates.end(), ranked);
    for (const auto& d:candidates) {
        bool suppressed=false;
        for (const auto& k:kept) {
            if (d.class_id==k.class_id && iou(d.xyxy,k.xyxy)>.7) { suppressed=true; break; }
        }
        if (!suppressed) kept.push_back(d);
        if (kept.size()==300) break;
    }
    return kept;
}
Selection select_asset(const std::vector<Detection>& detections, int expected) {
    if (expected<0 || expected>=14) throw std::invalid_argument("Invalid expected asset class");
    Selection result;
    const Detection* best=nullptr;
    for (const auto& d:detections) if (d.class_id==expected) {
        ++result.candidates;
        if (!best || ranked(d,*best)) best=&d;
    }
    if (best) {
        result.box=best->xyxy;
        for (float& v:result.box) v=std::max(0.0f,std::min(320.0f,v));
        result.confidence=best->confidence;
        result.fallback=false;
    }
    return result;
}
}
