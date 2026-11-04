#include "insplad/detection.h"
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void candidate(std::vector<float>& p,int index,int cls,float confidence,
                      float cx,float cy,float width,float height) {
    p[index]=cx;p[2100+index]=cy;p[4200+index]=width;p[6300+index]=height;
    p[(4+cls)*2100+index]=confidence;
}
int main() {
    try {
        std::vector<float> p(18*2100,0);
        candidate(p,0,7,.9f,100,100,80,80);
        candidate(p,1,7,.8f,101,101,80,80); // Same-class duplicate must be suppressed.
        candidate(p,2,5,.8f,101,101,80,80); // Other class must survive.
        candidate(p,3,7,.25f,250,250,10,10); // Strict confidence cutoff.
        const auto d=insplad::decode_nms(p.data(),p.size());
        require(d.size()==2,"NMS/confidence gate");
        require(d[0].class_id==7 && d[1].class_id==5,"Class-aware NMS");
        const auto missing=insplad::select_asset(d,9);
        require(missing.fallback && missing.box==insplad::InputBox{{0,0,320,320}},"Missing-class fallback");
        std::vector<insplad::Detection> tied(2);
        tied[0].xyxy={{100,10,200,300}};tied[1].xyxy={{-2,5,80,330}};
        for(auto& item:tied) { item.class_id=7; item.confidence=.8f; }
        tied[0].candidate=0;tied[1].candidate=1;
        const auto selected=insplad::select_asset(tied,7);
        require(!selected.fallback && selected.candidates==2,"Expected-class selection");
        require(selected.box==insplad::InputBox{{0,5,80,320}},"Lexicographic tie and clipped selection");
        std::cout << "PASS: confidence cutoff, class-aware suppression, fallback, tie order and clipping\n";
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';return 1;
    }
}
