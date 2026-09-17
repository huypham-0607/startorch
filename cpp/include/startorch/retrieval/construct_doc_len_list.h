#ifndef CONSTRUCT_DOC_LEN_LIST_H
#define CONSTRUCT_DOC_LEN_LIST_H

#include "startorch/utils/file_io.h"
#include <filesystem>
#include <vector>

void write_doc_len_entry(
    BufferedWriter& out_fp,
    const unsigned long long& delta,
    const unsigned int& freq
);

void write_doc_len_meta(
    const fs::path out_path,
    unsigned long long total_docs,
    unsigned long long total_frequency
);

const std::pair<unsigned long long, unsigned long long> read_doc_len_meta(
    const fs::path in_path
);

/**
 * @brief Construct doc_id - doc_len sequence (out_dir/doc_len_list.bin).
 *
 * Format: (doc_id<vbe_encoding>)(doc_len<unsigned int>)
 *
 * The token stream may arrive in any doc_id order, as long as each
 * document's tokens are contiguous. Entries are always written in ascending
 * doc_id order, skipping doc ids with no tokens.
 */
void construct_doc_len_list(
    const std::filesystem::path& in_dir,
    const std::filesystem::path& out_dir
);

/**
 * @brief Read doc_len_list.bin (a single file path, not a directory)
 * into a dense doc_id -> doc_len array.
 */
void read_doc_len_list(
    const std::filesystem::path& in_dir,
    std::vector<unsigned int>& doc_len_list
);

#endif
