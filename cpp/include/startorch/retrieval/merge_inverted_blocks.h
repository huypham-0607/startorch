#ifndef MERGE_INVERTED_BLOCKS_H
#define MERGE_INVERTED_BLOCKS_H

#include "startorch/retrieval/build_params.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Per-block BMW metadata.
 *
 * doc_id is this block's absolute start_doc_id - binary-searchable
 * directly, no reconstruction needed by callers. On disk, consecutive
 * blocks' start_doc_id are still delta+VBE encoded for space; the
 * delta<->absolute conversion happens at the serialization boundary
 * (write_block_meta_file encodes the delta, read_block_meta_file
 * reconstructs the absolute value), so every BlockMeta that ever exists in
 * memory - just-flushed or just-loaded - already holds an absolute doc_id.
 */
struct BlockMeta {
    unsigned long long doc_id;
    size_t start_addr;
    float block_ub;

    BlockMeta(
        unsigned long long _doc_id,
        size_t _start_addr,
        float _block_ub
    );
};

/**
 * @brief Per-term BMW metadata: the term-level upper bound plus the list
 * of this term's blocks.
 *
 * file_index lives here, not on BlockMeta: build_posting_list writes one
 * term's entire posting list in a single call, so every block it produces
 * for that term always lands in whichever posting_*.bin file was open at
 * the time - a term's blocks never span multiple files.
 *
 * end_addr is the exclusive end of this term's byte range in that file:
 * every (doc_id, freq) posting for this term lies in
 * [block_meta_list[0].start_addr, end_addr) - a strict half-open interval,
 * so end_addr itself is one past the last valid byte. Combined with the
 * first block's start_addr, this gives a hard, self-contained bound for
 * safe reads/seeks over this term's data without depending on adjacent
 * terms' placement.
 */
struct TermMeta {
    float term_ub;
    size_t end_addr;
    unsigned int doc_count;
    unsigned int file_index;
    std::vector<BlockMeta> block_meta_list;

    TermMeta();
};

/**
 * @brief Merge partial SPIMI blocks (construct_inverted_blocks output) into
 * a small, fixed number of final posting-data files plus one consolidated
 * BMW block-metadata file (out_dir / "block_meta.bin"), then metadata.
 *
 * Document lengths come in as arguments. build_index passes the lengths it
 * counted during the SPIMI pass; merge_inverted_blocks loads them from files.
 *
 * @param in_dir directory containing partial block_*.bin files
 * @param out_dir directory to write posting_*.bin, block_meta.bin and
 * metadata into
 * @param doc_len_list doc_id -> token count; its size is N
 * @param total_docs documents with at least one token
 * @param total_frequency tokens over all documents
 * @param params k1, b, block_size and split_size are used; validated here
 */
void merge_partial_blocks(
    const std::filesystem::path& in_dir,
    const std::filesystem::path& out_dir,
    const std::vector<unsigned int>& doc_len_list,
    const unsigned long long total_docs,
    const unsigned long long total_frequency,
    const BuildParams& params
);

/**
 * @brief merge_partial_blocks with document lengths loaded from
 * out_dir/doc_len_list.bin and out_dir/doc_len_meta.bin (construct_doc_len_list
 * output). For the standalone app and tests; the Python build uses build_index.
 */
void merge_inverted_blocks(
    const std::filesystem::path& in_dir,
    const std::filesystem::path& out_dir,
    const BuildParams& params = {}
);

// Positional form of the above, kept for existing callers. No defaults here:
// they live in BuildParams.
void merge_inverted_blocks(
    const std::filesystem::path& in_dir,
    const std::filesystem::path& out_dir,
    const float k1,
    const float b,
    const int block_size,
    const size_t split_size
);

std::vector<std::pair<std::string, TermMeta>> read_block_meta_file(
    const std::filesystem::path& in_path
);

// metadata.bin layout: magic "STMD", uint32 format version, then k1, b,
// avgdl (float), block_size (int), split_size (size_t). Format 1 (before
// 2026-09-19) began with three length-prefixed absolute paths instead.
constexpr unsigned int METADATA_FORMAT_VERSION = 2;

/**
 * @brief Write index build parameters in two forms: a plain-text key=value
 * file at out_path, one field per line - for humans to read/inspect, never
 * read back programmatically - and a binary twin at out_path with its
 * extension replaced by ".bin", the exact encoding read_metadata parses.
 *
 * No paths are stored: every index file sits next to metadata.bin, which
 * load_index relies on.
 *
 * @param out_path path to write the human-readable metadata file to; the
 * binary file is written alongside it with the same stem and a .bin extension
 * @param k1 BM25 k1 parameter the index was built with
 * @param b BM25 b parameter the index was built with
 * @param avgdl average document length the block upper bounds were built
 * with - the query engine must score with this exact value
 * @param block_size BMW block size the index was built with
 * @param split_size posting-file split threshold the index was built with
 */
void write_metadata(
    const std::filesystem::path& out_path,
    const float k1,
    const float b,
    const float avgdl,
    const int block_size,
    const size_t split_size
);

/**
 * @brief Read back the binary metadata file written by write_metadata (the
 * .bin twin, not the human-readable .txt), populating every out-parameter.
 * Throws std::runtime_error on a missing, truncated or older-format file
 * (the message says to rebuild the index).
 *
 * @param in_path path to the .bin metadata file to read
 */
void read_metadata(
    const std::filesystem::path& in_path,
    float& k1,
    float& b,
    float& avgdl,
    int& block_size,
    size_t& split_size
);

/**
 * @brief Everything load_index reads to open an index: the build parameters
 * from metadata.bin, plus every term's TermMeta from block_meta.bin.
 *
 * posting_dir is the folder holding metadata.bin, block_meta.bin,
 * doc_len_list.bin and posting_*.bin.
 */
struct IndexMeta {
    std::filesystem::path posting_dir;
    float k1;
    float b;
    float avgdl;
    int block_size;
    size_t split_size;
    std::vector<std::pair<std::string, TermMeta>> terms;
};

/**
 * @brief The one way to open an index: read metadata.bin at meta_path, then
 * block_meta.bin next to it. Every reader of an index (QueryEngine,
 * read_term_df_mapping) goes through here.
 *
 * Index files are resolved relative to meta_path's own folder: the merge
 * writes metadata into the posting folder itself, and metadata.bin stores no
 * paths, so a moved or renamed index folder still loads.
 *
 * @param meta_path path to the index's metadata.bin
 */
IndexMeta load_index(const std::filesystem::path& meta_path);

/**
 * @brief Return every indexed term with its document frequency, sorted by
 * descending df.
 *
 * @param meta_path path to the index's metadata.bin
 */
std::vector<std::pair<std::string, unsigned int>> read_term_df_mapping(
    const std::filesystem::path& meta_path
);

#endif
