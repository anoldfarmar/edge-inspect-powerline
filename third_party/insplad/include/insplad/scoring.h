#ifndef INSPLAD_SCORING_H
#define INSPLAD_SCORING_H
#include <cstddef>
#include <vector>
namespace insplad {
// FP32 storage, stable direct (q-b)^2 accumulation in FP32 (NEON on aarch64).
// No dot-product cancellation, approximation, or FP16. Query and bank are [N,64].
std::vector<float> nearest_euclidean(const float* query, std::size_t queries,
                                   const float* bank, std::size_t prototypes,
                                   std::size_t prototype_block = 256);
// Mean of the largest EIGHT patch 1NN Euclidean distances; FP64 sum of FP32 distances.
double top8_score(const std::vector<float>& distances);
}
#endif
