#ifndef BM25_H
#define BM25_H

/**
 * @brief BM25 tf/doc-length saturation term only - the (k1+1)tf /
 * (k1*((1-b)+b*(doc_len/avgdl)) + tf) piece, without the IDF factor.
 * Useful when df_t isn't known yet (e.g. while a term's posting list is
 * still being merged) and IDF needs to be applied later.
 */
float bm25_saturation(
    const float k1,
    const float b,
    const float tf,
    const float doc_len,
    const float avgdl
);

/**
 * @brief BM25 inverse document frequency:
 *
 * ln((N - df_t + 0.5) / (df_t + 0.5) + 1)
 *
 * The +1 keeps it strictly positive, even for a term present in every
 * document. The only IDF implementation - index build (block upper bounds)
 * and query time (real scores) must both call this, or pruning bounds stop
 * being true upper bounds.
 */
float bm25_idf(
    const unsigned long long N,
    const unsigned long long df_t
);

/**
 * @brief BM25 score for a single term-doc pair.
 *
 * bm25_idf(N, df_t) * (k1+1)*tf / (k1*((1-b) + b*(doc_len/avgdl)) + tf)
 */
float calc_BM25(
    const unsigned long long N,
    const unsigned long long df_t,
    const float tf,
    const float doc_len,
    const float avgdl,
    const float k1 = 1.2f,
    const float b = 0.75f
);

#endif
