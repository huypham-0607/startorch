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
 * @brief Token count per mapped doc id, built one token at a time.
 *
 * Mapped doc ids are dense in [0, N), so lengths live in an array indexed by
 * id, and tokens may arrive in any document order. Doc ids that never get a
 * token keep length 0 and are skipped when written.
 */
class DocLenCounter {
public:
    void add(const unsigned long long doc_id) {
        if (doc_id >= lengths.size()) lengths.resize(doc_id + 1, 0);
        ++lengths[doc_id];
        ++total_frequency;
    }

    // Documents with at least one token.
    unsigned long long total_docs() const;

    // Writes doc_len_list.bin and doc_len_meta.bin into out_dir.
    void write(const std::filesystem::path& out_dir) const;

    std::vector<unsigned int> lengths;
    unsigned long long total_frequency = 0;
};

/**
 * @brief Construct doc_id - doc_len sequence (out_dir/doc_len_list.bin) with
 * its totals (out_dir/doc_len_meta.bin), from a separate pass over the token
 * stream. build_index counts during the SPIMI pass instead.
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
