#ifndef FILE_NAMES_H
#define FILE_NAMES_H

#include <format>
#include <string>

/**
 * @brief Fixed on-disk filenames and naming patterns shared by the retrieval
 * pipeline's writers and readers (SPIMI construction, merge, query engine),
 * centralized here so the naming convention only needs to change in one
 * place instead of drifting independently across files.
 */
namespace file_names {
    inline constexpr const char* BLOCK_META = "block_meta.bin";
    inline constexpr const char* METADATA_TXT = "metadata.txt";
    inline constexpr const char* METADATA_BIN = "metadata.bin";
    inline constexpr const char* METADATA_BIN_EXTENSION = ".bin";
    inline constexpr const char* DOC_LEN_LIST = "doc_len_list.bin";
    inline constexpr const char* DOC_LEN_META = "doc_len_meta.bin";

    // Extension used when globbing a directory for partial SPIMI input
    // files (block_*.bin / token_*.bin) - matched by extension only, not
    // by the exact filename.
    inline constexpr const char* PARTIAL_BLOCK_EXT = ".bin";

    // posting_XXXX.bin - one of merge_inverted_blocks' final posting-data
    // output files, split by split_size.
    inline std::string posting_file_name(unsigned int file_index) {
        return std::format("posting_{:04}.bin", file_index);
    }

    // block_XXXX.bin - one partial index produced by a single SPIMI
    // construction pass (construct_inverted_blocks).
    inline std::string partial_block_file_name(unsigned int block_index) {
        return std::format("block_{:04}.bin", block_index);
    }
}

#endif
