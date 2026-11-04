#include "insplad/scoring.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

namespace insplad {
namespace {
float squared_distance(const float* a, const float* b) {
#ifdef __aarch64__
    float32x4_t s0=vdupq_n_f32(0),s1=s0,s2=s0,s3=s0;
    for (int c=0;c<64;c+=16) {
        const auto d0=vsubq_f32(vld1q_f32(a+c),vld1q_f32(b+c));
        const auto d1=vsubq_f32(vld1q_f32(a+c+4),vld1q_f32(b+c+4));
        const auto d2=vsubq_f32(vld1q_f32(a+c+8),vld1q_f32(b+c+8));
        const auto d3=vsubq_f32(vld1q_f32(a+c+12),vld1q_f32(b+c+12));
        s0=vfmaq_f32(s0,d0,d0);s1=vfmaq_f32(s1,d1,d1);
        s2=vfmaq_f32(s2,d2,d2);s3=vfmaq_f32(s3,d3,d3);
    }
    return vaddvq_f32(vaddq_f32(vaddq_f32(s0,s1),vaddq_f32(s2,s3)));
#else
    float sum=0;
    for(int c=0;c<64;++c) { const float d=a[c]-b[c];sum+=d*d; }
    return sum;
#endif
}
void validate(const float* data,std::size_t rows) {
    if(!data || !rows) throw std::invalid_argument("Empty scoring tensor");
    for(std::size_t i=0;i<rows*64;++i) if(!std::isfinite(data[i])) throw std::invalid_argument("Non-finite scoring tensor");
}
}
std::vector<float> nearest_euclidean(const float* q,std::size_t nq,const float* b,
                                   std::size_t nb,std::size_t block) {
    validate(q,nq);validate(b,nb);
    if(!block) block=nb;
    std::vector<float> distances(nq);
    #pragma omp parallel for schedule(static)
    for(std::ptrdiff_t i=0;i<static_cast<std::ptrdiff_t>(nq);++i) {
        float best=std::numeric_limits<float>::infinity();
        for(std::size_t start=0;start<nb;start+=block) {
            const auto end=std::min(start+block,nb);
            for(std::size_t j=start;j<end;++j) best=std::min(best,squared_distance(q+i*64,b+j*64));
        }
        distances[i]=std::sqrt(best);
    }
    for(float d:distances) if(!std::isfinite(d)) throw std::overflow_error("Non-finite nearest distance");
    return distances;
}
double top8_score(const std::vector<float>& d) {
    if(d.size()<8) throw std::invalid_argument("Top8 needs at least 8 patches");
    for(float x:d) if(!std::isfinite(x) || x<0) throw std::invalid_argument("Invalid patch distance");
    std::vector<float> sorted=d;
    std::partial_sort(sorted.begin(),sorted.begin()+8,sorted.end(),std::greater<float>());
    double sum=0;for(int i=0;i<8;++i) sum+=sorted[i];return sum/8;
}
}
