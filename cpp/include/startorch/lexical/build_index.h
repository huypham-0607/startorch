#ifndef BUILD_INDEX_H
#define BUILD_INDEX_H

#include "startorch/lexical/build_params.h"

#include <filesystem>

/**
 * @brief Build a complete index from a token stream, reading the stream once.
 *
 * 1. SPIMI writes partial blocks into partial_dir and counts every token
 *    toward its document's length during the same pass.
 * 2. doc_len_list.bin and doc_len_meta.bin are written into out_dir (the
 *    query engine loads doc_len_list.bin).
 * 3. The partial blocks are merged into out_dir with those lengths passed in
 *    directly, then block_meta.bin and metadata are written.
 *
 * Replaces running construct_doc_len_list, construct_inverted_blocks and
 * merge_inverted_blocks separately, which read the token stream twice and
 * relied on the merge finding the doc-length files in out_dir. Leftover
 * block_*.bin files in partial_dir are removed first: an earlier build that
 * produced more partial blocks would otherwise be merged into this one.
 *
 * @param token_dir directory of token_*.bin files, in any document order
 * @param partial_dir scratch directory for SPIMI partial blocks (created)
 * @param out_dir index directory (created)
 * @param params validated before anything is read
 */
void build_index(
    const std::filesystem::path& token_dir,
    const std::filesystem::path& partial_dir,
    const std::filesystem::path& out_dir,
    const BuildParams& params = {}
);

#endif
