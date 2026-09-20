#ifndef BUILD_PARAMS_H
#define BUILD_PARAMS_H

#include <cstddef>

/**
 * @brief Index build parameters. The only place their defaults live.
 *
 * The defaults are the OpenAlex profiles' values. Each profile in
 * project-config.toml sets its own; MS MARCO uses Anserini's k1 = 0.82,
 * b = 0.68.
 */
struct BuildParams {
    float k1 = 1.2f;                        // BM25 term-frequency saturation
    float b = 0.75f;                        // BM25 document-length normalization
    int block_size = 128;                   // postings per Block-Max WAND block
    size_t split_size = size_t(1) << 30;    // approximate max bytes per posting_*.bin
    size_t mem_limit = size_t(1) << 31;     // SPIMI memory budget per partial block

    /**
     * @brief Throw std::invalid_argument naming the first field out of range:
     * k1 in (0, 5], b in [0, 1], and block_size, split_size, mem_limit > 0.
     */
    void validate() const;
};

#endif
