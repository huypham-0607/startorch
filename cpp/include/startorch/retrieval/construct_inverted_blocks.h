#ifndef CONSTRUCT_INVERTED_BLOCKS_H
#define CONSTRUCT_INVERTED_BLOCKS_H

#include "startorch/retrieval/construct_doc_len_list.h"
#include "startorch/retrieval/posting_list.h"
#include "startorch/utils/file_io.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

bool build_partial_index(
    const SafeFile& token_stream,
    const size_t mem_limit,
    std::unordered_map<std::string, PostingList> &posting_list_mapping,
    std::vector<std::string> &dictionary
);

// The overload the build uses. When doc_lens is given, every token read is
// also counted toward its document's length.
bool build_partial_index(
    BufferedReader& token_stream,
    const size_t mem_limit,
    std::unordered_map<std::string, PostingList> &posting_list_mapping,
    std::vector<std::string> &dictionary,
    DocLenCounter* const doc_lens = nullptr
);

void write_partial_index(
    const std::filesystem::path out_file_path,
    std::unordered_map<std::string, PostingList>& posting_list_mapping,
    std::vector<std::string>& dictionary
);

/**
 * @brief SPIMI: chunk tokenized input (in_dir/token_*.bin) into partial,
 * per-shard inverted blocks (out_dir/block_*.bin), each built under
 * mem_limit bytes.
 *
 * The token stream may arrive in any doc_id order, as long as each
 * document's tokens are contiguous. Every posting list is sorted by doc_id
 * when written, and partial blocks may overlap in doc_id range -
 * merge_inverted_blocks' heap merge handles both.
 *
 * When doc_lens is given, document lengths are counted during this same
 * pass (see build_index), so no separate pass over the token stream is
 * needed for them.
 */
void construct_inverted_blocks(
    const std::filesystem::path& in_dir,
    const std::filesystem::path& out_dir,
    const size_t mem_limit,
    DocLenCounter* const doc_lens = nullptr
);

#endif
