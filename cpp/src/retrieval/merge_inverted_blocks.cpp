/**
 * @brief Merging partial posting blocks, into a final complete posting list.
 *
 * Each posting (1 single term) will be treated as an atomic unit. We will
 * group these posting units into blocks of certain sizes.
 *
 * Maintain a separate look-up list storing (file_id, position) tuple for fast
 * querying.
 *
 * To remind, Standard BM25 implementation is:
 *
 *  - IDF(Term) * tf(term,doc)(k+1) / tf(term,doc) + k (1-b+b(|d|/avgdl))
 *
 * For WAND scoring function \alpha_{t} * w(t,d)
 * - \alpha_{t} is our IDF: ln((N - df(Term) + 0.5) / (df(Term) + 0.5) + 1), see bm25_idf
 * - w(t,d) is tf(term,doc)(k+1) / tf(term,doc) + k (1-b+b(|d|/avgdl)).
 *
 * When querying with BMW, we will load all relevant
 */

#include "startorch/retrieval/merge_inverted_blocks.h"
#include "startorch/retrieval/file_names.h"
#include "startorch/retrieval/posting_list.h"
#include "startorch/retrieval/bm25.h"
#include "startorch/retrieval/construct_doc_len_list.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"
#include "startorch/utils/logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <optional>

namespace fs = std::filesystem;

namespace {
    // Read helpers for fields that must be present: the file ending here
    // means it is truncated, so a false return becomes an error.
    template <typename T>
    void read_field(BufferedReader& in, T& value) {
        if (!in.read(value)) {
            throw std::runtime_error(std::format("Failed to read file {}.", in.path().string()));
        }
    }

    void fread_field(BufferedReader& in, void* const dst, const size_t n, const size_t count) {
        if (!in.fread(dst, n, count)) {
            throw std::runtime_error(std::format("Failed to read file {}.", in.path().string()));
        }
    }

    void read_field_vbe(BufferedReader& in, unsigned long long& value) {
        if (!in.read_vbe(value)) {
            throw std::runtime_error(std::format("Failed to read file {}.", in.path().string()));
        }
    }

    constexpr char METADATA_MAGIC[4] = {'S', 'T', 'M', 'D'};
}

BlockMeta::BlockMeta(
    unsigned long long _doc_id,
    size_t _start_addr,
    float _block_ub
) :
doc_id(_doc_id),
start_addr(_start_addr),
block_ub(_block_ub) {}

TermMeta::TermMeta() : term_ub(0.0f), end_addr(0), doc_count(0), file_index(0) {}

// <term_size><posting_list_size><term><<vbe_encoding_{i}><freq_{i}>>
class Stream {
public:
    // Every partial block is open at once during the merge (234 on full-en),
    // so each gets a smaller buffer than a sequential pass would.
    Stream(const fs::path in_path) : in_file(in_path, STREAM_READ_BUFFER_SIZE) {
        is_empty = false;

        read_field(in_file, dict_size);
        if (dict_size == 0) {
            is_empty = true;
        }
        else {
            list_id = 0;
            read_list_header();

            // Setting up first posting_item (posting list should be non-empty)
            item_id = 0;
            doc_id = 0;
            read_item();
        }
    }

    bool empty() const {
        return is_empty;
    }

    PostingItem get_item() const {
        if (is_empty) {
            throw std::runtime_error(std::format(
                "Accessing item in an empty Stream"
            ));
        }
        return PostingItem(doc_id, freq);
    }

    unsigned int get_item_id() const {
        if (is_empty) {
            throw std::runtime_error(std::format(
                "Accessing item_id in an empty Stream"
            ));
        }
        return item_id;
    }

    unsigned int get_list_size() const {
        if (is_empty) {
            throw std::runtime_error(std::format(
                "Accessing list_size in an empty Stream"
            ));
        }
        return list_size;
    }

    const std::string& get_term() const {
        if (is_empty) {
            throw std::runtime_error(std::format(
                "Accessing term in an empty Stream"
            ));
        }
        return term;
    }

    unsigned int get_list_id() const {
        if (is_empty) {
            throw std::runtime_error(std::format(
                "Accessing list_id in an empty Stream"
            ));
        }
        return list_id;
    }

    unsigned int get_dict_size() const {
        return dict_size;
    }

    /*
     * Returns true if next item is available (not empty).
     */
    bool next() {
        if (is_empty) return false;
        ++item_id;
        if (item_id == list_size) {
            ++list_id;
            if (list_id == dict_size) {
                is_empty = true;
                return false;
            }

            read_list_header();
            item_id = 0;
            doc_id = 0;
        }

        read_item();
        return true;
    }

private:
    BufferedReader in_file;
    bool is_empty;
    unsigned int dict_size;

    unsigned int list_id;
    unsigned int list_size;
    std::string term;

    unsigned int item_id;
    unsigned long long doc_id;
    unsigned int freq;

    void read_list_header() {
        unsigned short term_size;
        read_field(in_file, term_size);
        read_field(in_file, list_size);
        term.resize(term_size);
        fread_field(in_file, term.data(), sizeof(char), term_size);
    }

    void read_item() {
        unsigned long long delta;
        read_field_vbe(in_file, delta);
        doc_id += delta;
        read_field(in_file, freq);
    }
};

/**
 * @brief Write buffer's postings (delta-encoded from 0, i.e. resetting the
 * delta base at this block boundary - required for start_addr to be
 * independently seekable) to out_file, compute this block's metadata, and
 * clear buffer.
 *
 * block_ub is left as the saturation-only term (see bm25_saturation) -
 * the caller multiplies in IDF once df_t is known. BlockMeta.doc_id is
 * this block's absolute start_doc_id (delta-encoding only happens later,
 * at serialization - see write_block_meta_file).
 *
 * @param bytes_written accumulator, incremented by the bytes actually written.
 */
BlockMeta flush_buffer(
    PostingList& buffer,
    BufferedWriter& out_file,
    const std::vector<unsigned int>& doc_len_list,
    const float avgdl,
    const float k1,
    const float b,
    size_t& bytes_written
) {
    unsigned long long start_doc_id = buffer[0].doc_id;

    size_t start_addr = (size_t)out_file.ftell();

    float max_saturation = 0.0f;
    unsigned long long last = 0;
    unsigned char vbe_buffer[BUFFER_LIMIT];

    for (size_t i = 0; i < buffer.size(); i++) {
        const PostingItem& item = buffer[i];
        unsigned long long posting_delta = item.doc_id - last;
        int encode_length = vbe_encode(posting_delta, vbe_buffer);
        out_file.fwrite(vbe_buffer, sizeof(unsigned char), encode_length);
        out_file.write(item.freq);
        last = item.doc_id;
        bytes_written += encode_length + sizeof(item.freq);

        float doc_len = (float)doc_len_list[item.doc_id];
        float saturation = bm25_saturation(k1, b, (float)item.freq, doc_len, avgdl);
        max_saturation = std::max(max_saturation, saturation);
    }

    buffer.clear();

    return BlockMeta(start_doc_id, start_addr, max_saturation);
}

size_t build_posting_list(
    const std::string& term,
    const std::vector<int>& valid_streams,
    std::vector<Stream>& streams,
    const std::vector<unsigned int>& doc_len_list,
    BufferedWriter& out_file,
    const unsigned int file_index,
    std::unordered_map<std::string,TermMeta>& term_meta_mapping,
    const int block_size,
    const float avgdl,
    const float k1,
    const float b,
    const unsigned long long N
) {
    std::priority_queue<
        std::pair<PostingItem, int>,
        std::vector<std::pair<PostingItem, int>>,
        std::greater<std::pair<PostingItem,int>>
    > item_heap;

    for (auto i:valid_streams) {
        if (!streams[i].empty() && streams[i].get_term() == term) {
            item_heap.push(std::make_pair(streams[i].get_item(),i));
        }
    }

    TermMeta& term_meta = term_meta_mapping[term];
    term_meta.file_index = file_index;
    PostingList buffer;
    size_t total_size = 0;

    // A single (doc_id, term) pair can arrive from the heap twice:
    // build_partial_index's memory-limit check is per-token, not
    // per-document, so a document's tokens for this term can be split
    // across two separate SPIMI partial blocks. Those must be merged
    // (has_document/add_document, and the merge has to happen before the
    // block_size flush check.
    while (!item_heap.empty()){
        auto [item, stream_id] = item_heap.top();

        bool is_new_doc = !buffer.has_document(item.doc_id);

        if (is_new_doc && (int)buffer.size() == block_size) {
            term_meta.block_meta_list.push_back(flush_buffer(
                buffer, out_file, doc_len_list, avgdl, k1, b,
                total_size
            ));
        }

        item_heap.pop();
        buffer.add_document(item.doc_id, item.freq);
        if (is_new_doc) {
            ++term_meta.doc_count;
        }

        // A stream is still on this term until next() moves it to its next
        // posting list, which an integer compare detects without copying or
        // comparing the term string once per posting.
        const unsigned int list_id = streams[stream_id].get_list_id();
        streams[stream_id].next();
        if (!streams[stream_id].empty() && streams[stream_id].get_list_id() == list_id) {
            item_heap.push(std::make_pair(streams[stream_id].get_item(),stream_id));
        }
    }

    if (buffer.size() > 0) {
        term_meta.block_meta_list.push_back(flush_buffer(
            buffer, out_file, doc_len_list, avgdl, k1, b,
            total_size
        ));
    }

    // Exclusive end of this term's byte range - every write for this term
    // just finished, so the file cursor is now one past its last byte.
    term_meta.end_addr = (size_t)out_file.ftell();

    // df_t (term_meta.doc_count) is only known now that the term's heap is
    // fully drained - apply IDF retroactively to every block this term
    // just produced, turning saturation-only block_ub into the full BM25
    // upper bound, and derive term_ub as the max over them.
    float idf = bm25_idf(N, term_meta.doc_count);
    float max_full_ub = 0.0f;
    for (BlockMeta& block : term_meta.block_meta_list) {
        block.block_ub *= idf;
        max_full_ub = std::max(max_full_ub, block.block_ub);
    }
    term_meta.term_ub = max_full_ub;

    return total_size;
}

/**
 * @brief Serialize term_meta_mapping into a single consolidated file:
 * for each term, (term_size, term_bytes, term_ub, end_addr, doc_count,
 * file_index, block_count), followed by block_count blocks of
 * (delta<vbe>, start_addr, block_ub).
 *
 * file_index is written once per term, not once per block - every block
 * belonging to a term lands in the same posting_*.bin file, since
 * build_posting_list writes a whole term in one call (see TermMeta).
 *
 * No leading term count - read_block_meta_file reads until a clean EOF.
 */
void write_block_meta_file(
    const fs::path& out_path,
    const std::unordered_map<std::string, TermMeta>& term_meta_mapping
) {
    BufferedWriter out_file(out_path);

    for (const auto& [term, term_meta] : term_meta_mapping) {
        unsigned short term_size = term.size();
        out_file.write(term_size);
        out_file.fwrite(term.c_str(), sizeof(char), term_size);

        out_file.write(term_meta.term_ub);
        out_file.write(term_meta.end_addr);
        out_file.write(term_meta.doc_count);
        out_file.write(term_meta.file_index);

        unsigned int block_count = term_meta.block_meta_list.size();
        out_file.write(block_count);

        unsigned char vbe_buffer[BUFFER_LIMIT];
        unsigned long long prev_start_doc_id = 0;
        for (const BlockMeta& block : term_meta.block_meta_list) {
            unsigned long long delta = block.doc_id - prev_start_doc_id;
            prev_start_doc_id = block.doc_id;

            int encode_length = vbe_encode(delta, vbe_buffer);
            out_file.fwrite(vbe_buffer, sizeof(unsigned char), encode_length);
            out_file.write(block.start_addr);
            out_file.write(block.block_ub);
        }
    }
}

std::vector<std::pair<std::string, TermMeta>> read_block_meta_file(
    const fs::path& in_path
) {
    BufferedReader in_file(in_path);
    std::vector<std::pair<std::string, TermMeta>> result;

    while (true) {
        unsigned short term_size;
        if (!in_file.read(term_size)) break;

        std::string term(term_size, '\0');
        fread_field(in_file, term.data(), sizeof(char), term_size);

        TermMeta term_meta;
        read_field(in_file, term_meta.term_ub);
        read_field(in_file, term_meta.end_addr);
        read_field(in_file, term_meta.doc_count);
        read_field(in_file, term_meta.file_index);

        unsigned int block_count;
        read_field(in_file, block_count);
        term_meta.block_meta_list.reserve(block_count);

        unsigned long long cur_start_doc_id = 0;
        for (unsigned int i = 0; i < block_count; i++) {
            unsigned long long delta;
            read_field_vbe(in_file, delta);
            cur_start_doc_id += delta;

            size_t start_addr;
            float block_ub;
            read_field(in_file, start_addr);
            read_field(in_file, block_ub);

            term_meta.block_meta_list.emplace_back(cur_start_doc_id, start_addr, block_ub);
        }

        result.emplace_back(std::move(term), std::move(term_meta));
    }

    return result;
}

void write_metadata(
    const fs::path& out_path,
    const float k1,
    const float b,
    const float avgdl,
    const int block_size,
    const size_t split_size
) {
    // Human-readable copy, for inspection only - never read back by
    // read_metadata.
    SafeFile txt_file(out_path, "w");
    const std::string text = std::format(
        "format_version={}\nk1={}\nb={}\navgdl={}\nblock_size={}\nsplit_size={}\n",
        METADATA_FORMAT_VERSION, k1, b, avgdl, block_size, split_size
    );
    if (std::fputs(text.c_str(), txt_file.get()) < 0) {
        throw std::runtime_error(std::format(
            "Failed to write metadata to {}",
            out_path.string()
        ));
    }

    // Binary twin - authoritative, exact, what read_metadata actually parses:
    // magic, format version, then the parameters. No paths: every index file
    // sits next to metadata.bin (see load_index).
    fs::path bin_path = out_path;
    bin_path.replace_extension(file_names::METADATA_BIN_EXTENSION);
    SafeFile bin_file(bin_path, "wb");

    char magic[sizeof(METADATA_MAGIC)];
    std::memcpy(magic, METADATA_MAGIC, sizeof(magic));
    unsigned int version = METADATA_FORMAT_VERSION;
    float k1_v = k1, b_v = b, avgdl_v = avgdl;
    int block_size_v = block_size;
    size_t split_size_v = split_size;

    bin_file.fwrite(magic, sizeof(char), sizeof(magic));
    bin_file.fwrite(&version, sizeof(version), 1);
    bin_file.fwrite(&k1_v, sizeof(k1_v), 1);
    bin_file.fwrite(&b_v, sizeof(b_v), 1);
    bin_file.fwrite(&avgdl_v, sizeof(avgdl_v), 1);
    bin_file.fwrite(&block_size_v, sizeof(block_size_v), 1);
    bin_file.fwrite(&split_size_v, sizeof(split_size_v), 1);
}

void read_metadata(
    const fs::path& in_path,
    float& k1,
    float& b,
    float& avgdl,
    int& block_size,
    size_t& split_size
) {
    BufferedReader in_file(in_path, MIN_READ_BUFFER_SIZE * 4);

    char magic[sizeof(METADATA_MAGIC)];
    if (!in_file.read(magic) || std::memcmp(magic, METADATA_MAGIC, sizeof(magic)) != 0) {
        throw std::runtime_error(std::format(
            "{} is not a format {} metadata file. It was probably written before 2026-09-19 "
            "(format 1, which stored index paths). Rebuild the index.",
            in_path.string(), METADATA_FORMAT_VERSION
        ));
    }

    unsigned int version;
    read_field(in_file, version);
    if (version != METADATA_FORMAT_VERSION) {
        throw std::runtime_error(std::format(
            "{} has metadata format {}, but this build reads format {}. Rebuild the index.",
            in_path.string(), version, METADATA_FORMAT_VERSION
        ));
    }

    read_field(in_file, k1);
    read_field(in_file, b);
    read_field(in_file, avgdl);
    read_field(in_file, block_size);
    read_field(in_file, split_size);
}

void merge_partial_blocks(
    const fs::path& in_dir,
    const fs::path& out_dir,
    const std::vector<unsigned int>& doc_len_list,
    const unsigned long long total_docs,
    const unsigned long long total_frequency,
    const BuildParams& params
) {
    params.validate();
    Logger logger(__FILE_NAME__, Logger::INFO);

    // The single avgdl for this index: total tokens over documents that
    // produced at least one token. doc_len_list.size() is not used here -
    // it also counts doc_id gaps (documents with no tokens). Written to
    // metadata below, so the query engine scores with exactly the value
    // these block upper bounds were built with.
    const float avgdl = static_cast<float>(
        static_cast<double>(total_frequency) / static_cast<double>(total_docs)
    );
    unsigned long long N = doc_len_list.size();

    std::vector<fs::path> in_paths = glob_files(in_dir, "", file_names::PARTIAL_BLOCK_EXT);
    sort(in_paths.begin(), in_paths.end());


    std::vector<Stream> streams;
    streams.reserve(in_paths.size());
    std::priority_queue<
        std::pair<std::string, int>,
        std::vector<std::pair<std::string, int>>,
        std::greater<std::pair<std::string,int>>
    > string_heap;

    for (int i = 0; i < in_paths.size(); i++){
        streams.push_back(Stream(in_paths[i]));
        if (!streams[i].empty()) {
            string_heap.push(std::make_pair(streams[i].get_term(),i));
        }
    }

    logger.log("Started merging inverted index blocks.");

    std::unordered_map<std::string, TermMeta> term_meta_mapping;
    size_t cur_disk_usage = 0;
    unsigned int file_index = 0;

    std::optional<BufferedWriter> out_file;
    out_file.emplace(out_dir / file_names::posting_file_name(file_index));

    while (!string_heap.empty()) {
        std::string term = string_heap.top().first;
        std::vector<int> valid_streams;

        while (!string_heap.empty() && string_heap.top().first == term) {
            valid_streams.push_back(string_heap.top().second);
            string_heap.pop();
        }

        size_t disk_required = build_posting_list(
            term,
            valid_streams,
            streams,
            doc_len_list,
            *out_file,
            file_index,
            term_meta_mapping,
            params.block_size,
            avgdl,
            params.k1,
            params.b,
            N
        );

        // Streams that still have data after being drained past `term` need
        // to go back into the heap under their new current term - otherwise
        // every stream only ever contributes its first term.
        for (int i : valid_streams) {
            if (!streams[i].empty()) {
                string_heap.push(std::make_pair(streams[i].get_term(), i));
            }
        }

        if (cur_disk_usage && cur_disk_usage + disk_required > params.split_size) {
            ++file_index;
            cur_disk_usage = 0;
            out_file.emplace(out_dir / file_names::posting_file_name(file_index));
        }

        cur_disk_usage += disk_required;
    }
    out_file.reset();

    write_block_meta_file(out_dir / file_names::BLOCK_META, term_meta_mapping);

    logger.log("Finished merging inverted index blocks. Writing metadata...");

    write_metadata(
        out_dir / file_names::METADATA_TXT,
        params.k1,
        params.b,
        avgdl,
        params.block_size,
        params.split_size
    );

    logger.log("Finished writing metadata.");
}

void merge_inverted_blocks(
    const fs::path& in_dir,
    const fs::path& out_dir,
    const BuildParams& params
) {
    params.validate();

    std::vector<unsigned int> doc_len_list;
    read_doc_len_list(out_dir / file_names::DOC_LEN_LIST, doc_len_list);
    const auto [total_docs, total_frequency] = read_doc_len_meta(out_dir / file_names::DOC_LEN_META);

    merge_partial_blocks(in_dir, out_dir, doc_len_list, total_docs, total_frequency, params);
}

void merge_inverted_blocks(
    const fs::path& in_dir,
    const fs::path& out_dir,
    const float k1,
    const float b,
    const int block_size,
    const size_t split_size
) {
    BuildParams params;
    params.k1 = k1;
    params.b = b;
    params.block_size = block_size;
    params.split_size = split_size;
    merge_inverted_blocks(in_dir, out_dir, params);
}

IndexMeta load_index(const fs::path& meta_path) {
    IndexMeta index;
    read_metadata(meta_path, index.k1, index.b, index.avgdl, index.block_size, index.split_size);

    // Every index file sits next to metadata.bin.
    index.posting_dir = meta_path.parent_path();
    index.terms = read_block_meta_file(index.posting_dir / file_names::BLOCK_META);
    return index;
}

std::vector<std::pair<std::string, unsigned int>> read_term_df_mapping (
    const fs::path& meta_path
) {
    Logger logger(__FILE_NAME__, Logger::INFO);

    logger.log(std::format("Fetching TermMeta from {}.", meta_path.string()));
    IndexMeta index = load_index(meta_path);
    logger.log(std::format("Finished fetching TermMeta from {}.", meta_path.string()));

    std::vector<std::pair<std::string, unsigned int>> results;
    results.reserve(index.terms.size());
    for (auto& [term, metadata] : index.terms) {
        results.emplace_back(std::move(term), metadata.doc_count);
    }

    logger.log(std::format("Sorting (term, df) mapping..."));

    std::sort(results.begin(), results.end(), [&] (
        const std::pair<std::string, unsigned int>& a, const std::pair<std::string, unsigned int>& b) {
            return a.second > b.second;
        }
    );
    logger.log(std::format("Finished sorting (term, df) mapping."));

    return results;
}
