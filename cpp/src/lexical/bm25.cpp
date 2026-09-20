#include "startorch/lexical/bm25.h"

#include <cmath>

float bm25_saturation(
    const float k1,
    const float b,
    const float tf,
    const float doc_len,
    const float avgdl
) {
    float numerator = (k1 + 1.0f) * tf;
    float denominator = k1 * ((1.0f - b) + b * (doc_len / avgdl)) + tf;
    return numerator / denominator;
}

float bm25_idf(
    const unsigned long long N,
    const unsigned long long df_t
) {
    // Double precision: N reaches hundreds of millions, past what a float
    // represents exactly. Converting before subtracting also avoids
    // unsigned underflow. log1p(x) == ln(x + 1), more accurate for small x.
    const double n = static_cast<double>(N);
    const double df = static_cast<double>(df_t);
    return static_cast<float>(std::log1p((n - df + 0.5) / (df + 0.5)));
}

float calc_BM25(
    const unsigned long long N,
    const unsigned long long df_t,
    const float tf,
    const float doc_len,
    const float avgdl,
    const float k1,
    const float b
) {
    return bm25_idf(N, df_t) * bm25_saturation(k1, b, tf, doc_len, avgdl);
}
