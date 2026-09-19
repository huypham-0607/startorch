#include "startorch/retrieval/bm25.h"
#include "startorch/retrieval/build_params.h"

#include <cmath>
#include <gtest/gtest.h>

TEST(BM25SaturationTest, AtAverageDocLengthSimplifiesToClassicSaturation) {
    // doc_len == avgdl makes the length-normalization term (1-b)+b*(dl/avgdl)
    // collapse to exactly 1, so saturation should reduce to (k1+1)*tf/(k1+tf).
    float k1 = 1.2f, b = 0.75f, tf = 3.0f, avgdl = 10.0f;
    float expected = (k1 + 1.0f) * tf / (k1 + tf);
    ASSERT_NEAR(bm25_saturation(k1, b, tf, /*doc_len=*/avgdl, avgdl), expected, 1e-5);
}

TEST(BM25SaturationTest, BZeroMakesDocLengthIrrelevant) {
    // b=0 disables length normalization entirely - doc_len shouldn't affect
    // the result at all.
    float k1 = 1.2f, b = 0.0f, tf = 2.0f, avgdl = 1.0f;
    float short_doc = bm25_saturation(k1, b, tf, /*doc_len=*/1.0f, avgdl);
    float long_doc = bm25_saturation(k1, b, tf, /*doc_len=*/999999.0f, avgdl);
    ASSERT_NEAR(short_doc, long_doc, 1e-5);
}

TEST(BM25SaturationTest, ExactHandComputedValue) {
    // k1=2, b=1, tf=1, doc_len=4, avgdl=2:
    // denom = 2*((1-1) + 1*(4/2)) + 1 = 2*2 + 1 = 5
    // num   = (2+1)*1 = 3
    // saturation = 3/5 = 0.6
    ASSERT_NEAR(bm25_saturation(2.0f, 1.0f, 1.0f, 4.0f, 2.0f), 0.6f, 1e-6);
}

TEST(BM25SaturationTest, MonotonicIncreasingInTfWithDiminishingReturns) {
    float k1 = 1.2f, b = 0.75f, doc_len = 5.0f, avgdl = 5.0f;
    float s1 = bm25_saturation(k1, b, 1.0f, doc_len, avgdl);
    float s2 = bm25_saturation(k1, b, 2.0f, doc_len, avgdl);
    float s3 = bm25_saturation(k1, b, 3.0f, doc_len, avgdl);
    float s4 = bm25_saturation(k1, b, 4.0f, doc_len, avgdl);

    ASSERT_LT(s1, s2);
    ASSERT_LT(s2, s3);
    ASSERT_LT(s3, s4);

    // Saturating curve: each successive increment should be smaller than the last.
    ASSERT_LT(s4 - s3, s3 - s2);
    ASSERT_LT(s3 - s2, s2 - s1);
}

TEST(BM25SaturationTest, MonotonicDecreasingInDocLength) {
    // Longer-than-average documents should be penalized (lower saturation
    // for the same tf), when b > 0.
    float k1 = 1.2f, b = 0.75f, tf = 2.0f, avgdl = 10.0f;
    float short_doc = bm25_saturation(k1, b, tf, /*doc_len=*/5.0f, avgdl);
    float avg_doc = bm25_saturation(k1, b, tf, /*doc_len=*/10.0f, avgdl);
    float long_doc = bm25_saturation(k1, b, tf, /*doc_len=*/20.0f, avgdl);

    ASSERT_GT(short_doc, avg_doc);
    ASSERT_GT(avg_doc, long_doc);
}

TEST(BM25IdfTest, ExactHandComputedValue) {
    // N=100, df=50: (100-50+0.5)/(50+0.5) + 1 = 1 + 1 = 2, so IDF = ln(2).
    ASSERT_NEAR(bm25_idf(100, 50), std::log(2.0), 1e-6);
}

TEST(BM25IdfTest, StaysPositiveWhenTermInEveryDocument) {
    // df_t == N: the +1 keeps IDF strictly positive instead of zero.
    // (100-100+0.5)/(100+0.5) + 1 = 1 + 0.5/100.5.
    float idf = bm25_idf(100, 100);
    ASSERT_GT(idf, 0.0f);
    ASSERT_NEAR(idf, std::log1p(0.5 / 100.5), 1e-7);
}

TEST(BM25IdfTest, MonotonicDecreasingInDocumentFrequency) {
    ASSERT_GT(bm25_idf(1000, 1), bm25_idf(1000, 10));
    ASSERT_GT(bm25_idf(1000, 10), bm25_idf(1000, 500));
    ASSERT_GT(bm25_idf(1000, 500), bm25_idf(1000, 1000));
}

TEST(BM25IdfTest, ExactAtCorpusScale) {
    // N past float's exact-integer range (2^24): double precision must keep
    // IDF accurate. N=345,000,000, df=1 -> ln((N - 0.5)/1.5 + 1).
    const double N = 345000000.0;
    ASSERT_NEAR(bm25_idf(345000000ull, 1), std::log1p((N - 0.5) / 1.5), 1e-5);
}

TEST(CalcBM25Test, ScoreStaysPositiveWhenTermInEveryDocument) {
    // df_t == N still carries a small positive IDF, so the score is
    // positive rather than zero.
    const BuildParams params;
    float score = calc_BM25(/*N=*/100, /*df_t=*/100, /*tf=*/50.0f, /*doc_len=*/3.0f, /*avgdl=*/10.0f, params.k1, params.b);
    ASSERT_GT(score, 0.0f);
}

TEST(CalcBM25Test, EqualsIdfTimesSaturation) {
    unsigned long long N = 100, df_t = 50;
    float tf = 3.0f, doc_len = 10.0f, avgdl = 10.0f, k1 = 1.2f, b = 0.75f;

    // Documented formula, written out independently of bm25_idf.
    float expected_idf = std::log(((double)N - df_t + 0.5) / (df_t + 0.5) + 1.0);
    float expected = expected_idf * bm25_saturation(k1, b, tf, doc_len, avgdl);

    ASSERT_NEAR(calc_BM25(N, df_t, tf, doc_len, avgdl, k1, b), expected, 1e-5);
}

TEST(CalcBM25Test, RarerTermsScoreHigherAllElseEqual) {
    float tf = 2.0f, doc_len = 8.0f, avgdl = 8.0f;
    const BuildParams params;
    float common = calc_BM25(/*N=*/1000, /*df_t=*/500, tf, doc_len, avgdl, params.k1, params.b);
    float rare = calc_BM25(/*N=*/1000, /*df_t=*/5, tf, doc_len, avgdl, params.k1, params.b);

    ASSERT_GT(rare, common);
}
