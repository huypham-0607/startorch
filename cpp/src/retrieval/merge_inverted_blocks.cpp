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
#include <format>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <optional>

namespace fs = std::filesystem;

constexpr size_t BUF_SIZE = (1<<20);

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
    Stream(const fs::path in_path) : in_file(SafeFile(in_path, "rb")) {
        is_empty = false;

        in_file.fread(&dict_size, sizeof(dict_size), 1);
        if (dict_size == 0) {
            is_empty = true;
        }
        else {
            list_id = 0;

            // Setting up first posting_list
            unsigned short term_size;
            in_file.fread(&term_size, sizeof(term_size), 1);
            in_file.fread(&list_size, sizeof(list_size), 1);
            term.clear(); term.resize(term_size);
            in_file.fread(&(term[0]), sizeof(char), term_size);

            // Setting up first posting_item (posting list should be non-empty)
            item_id = 0;
            doc_id = 0;
            unsigned char buffer[BUFFER_LIMIT];
            read_vbe(in_file.get(), buffer);
            doc_id += vbe_decode(buffer);
            in_file.fread(&freq, sizeof(freq), 1);
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

    std::string get_term() const {
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

            unsigned short term_size;
            in_file.fread(&term_size, sizeof(term_size), 1);
            in_file.fread(&list_size, sizeof(list_size), 1);
            term.clear(); term.resize(term_size);
            in_file.fread(&(term[0]), sizeof(char), term_size);

            item_id = 0;
            doc_id = 0;
        }

        unsigned char buffer[BUFFER_LIMIT];
        read_vbe(in_file.get(), buffer);
        doc_id += vbe_decode(buffer);
        in_file.fread(&freq, sizeof(freq), 1);
        return true;
    }

private:
    SafeFile in_file;
    bool is_empty;
    unsigned int dict_size;

    unsigned int list_id;
    unsigned int list_size;
    std::string term;

    unsigned int item_id;
    unsigned long long doc_id;
    unsigned int freq;
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
        out_file.fwrite(&item.freq, sizeof(item.freq), 1);
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

        streams[stream_id].next();
        if (!streams[stream_id].empty() && streams[stream_id].get_term() == term) {
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
    BufferedWriter out_file(out_path, BUF_SIZE);

    for (const auto& [term, term_meta] : term_meta_mapping) {
        unsigned short term_size = term.size();
        out_file.fwrite(&term_size, sizeof(term_size), 1);
        out_file.fwrite(term.c_str(), sizeof(char), term_size);

        out_file.fwrite(&term_meta.term_ub, sizeof(term_meta.term_ub), 1);
        out_file.fwrite(&term_meta.end_addr, sizeof(term_meta.end_addr), 1);
        out_file.fwrite(&term_meta.doc_count, sizeof(term_meta.doc_count), 1);
        out_file.fwrite(&term_meta.file_index, sizeof(term_meta.file_index), 1);

        unsigned int block_count = term_meta.block_meta_list.size();
        out_file.fwrite(&block_count, sizeof(block_count), 1);

        unsigned char vbe_buffer[BUFFER_LIMIT];
        unsigned long long prev_start_doc_id = 0;
        for (const BlockMeta& block : term_meta.block_meta_list) {
            unsigned long long delta = block.doc_id - prev_start_doc_id;
            prev_start_doc_id = block.doc_id;

            int encode_length = vbe_encode(delta, vbe_buffer);
            out_file.fwrite(vbe_buffer, sizeof(unsigned char), encode_length);
            out_file.fwrite(&block.start_addr, sizeof(block.start_addr), 1);
            out_file.fwrite(&block.block_ub, sizeof(block.block_ub), 1);
        }
    }
}

std::vector<std::pair<std::string, TermMeta>> read_block_meta_file(
    const fs::path& in_path
) {
    SafeFile in_file(in_path, "rb");
    std::vector<std::pair<std::string, TermMeta>> result;

    while (true) {
        unsigned short term_size;
        if (!in_file.fread(&term_size, sizeof(term_size), 1, false)) break;

        std::string term(term_size, '\0');
        in_file.fread(&term[0], sizeof(char), term_size);

        TermMeta term_meta;
        in_file.fread(&term_meta.term_ub, sizeof(term_meta.term_ub), 1);
        in_file.fread(&term_meta.end_addr, sizeof(term_meta.end_addr), 1);
        in_file.fread(&term_meta.doc_count, sizeof(term_meta.doc_count), 1);
        in_file.fread(&term_meta.file_index, sizeof(term_meta.file_index), 1);

        unsigned int block_count;
        in_file.fread(&block_count, sizeof(block_count), 1);

        unsigned long long cur_start_doc_id = 0;
        for (unsigned int i = 0; i < block_count; i++) {
            unsigned char vbe_buffer[BUFFER_LIMIT];
            read_vbe(in_file.get(), vbe_buffer);
            cur_start_doc_id += vbe_decode(vbe_buffer);

            size_t start_addr;
            float block_ub;
            in_file.fread(&start_addr, sizeof(start_addr), 1);
            in_file.fread(&block_ub, sizeof(block_ub), 1);

            term_meta.block_meta_list.push_back(BlockMeta(cur_start_doc_id, start_addr, block_ub));
        }

        result.push_back({term, term_meta});
    }

    return result;
}

void write_metadata(
    const fs::path& out_path,
    const fs::path& posting_dir,
    const fs::path& doc_len_dir,
    const fs::path& doc_len_meta_dir,
    const float k1,
    const float b,
    const float avgdl,
    const int block_size,
    const size_t split_size
) {
    // Human-readable copy, for inspection only - never read back by
    // read_metadata. "%f" truncates to 6 digits after the decimal, which
    // does not round-trip an IEEE-754 float exactly in general.
    SafeFile txt_file(out_path, "w");
    int res = 0;

    res = (res || (std::fprintf(txt_file.get(), "posting_dir=%s\n", posting_dir.string().c_str()) < 0));
    res = (res || (std::fprintf(txt_file.get(), "doc_len_dir=%s\n", doc_len_dir.string().c_str()) < 0));
    res = (res || (std::fprintf(txt_file.get(), "doc_len_meta_dir=%s\n", doc_len_meta_dir.string().c_str()) < 0));
    res = (res || (std::fprintf(txt_file.get(), "k1=%f\n", k1) < 0));
    res = (res || (std::fprintf(txt_file.get(), "b=%f\n", b) < 0));
    res = (res || (std::fprintf(txt_file.get(), "avgdl=%f\n", avgdl) < 0));
    res = (res || (std::fprintf(txt_file.get(), "block_size=%d\n", block_size) < 0));
    res = (res || (std::fprintf(txt_file.get(), "split_size=%zu\n", split_size) < 0));

    if (res) {
        throw std::runtime_error(std::format(
            "Failed to write metadata to {}",
            out_path.string()
        ));
    }

    // Binary twin - authoritative, exact, what read_metadata actually
    // parses. Same length-prefixed-string convention as write_block_meta_file
    // uses for terms.
    fs::path bin_path = out_path;
    bin_path.replace_extension(file_names::METADATA_BIN_EXTENSION);
    SafeFile bin_file(bin_path, "wb");

    std::string posting_dir_str = posting_dir.string();
    unsigned short posting_dir_len = posting_dir_str.size();
    fwrite(&posting_dir_len, sizeof(posting_dir_len), 1, bin_file.get());
    fwrite(posting_dir_str.c_str(), sizeof(char), posting_dir_len, bin_file.get());

    std::string doc_len_dir_str = doc_len_dir.string();
    unsigned short doc_len_dir_len = doc_len_dir_str.size();
    fwrite(&doc_len_dir_len, sizeof(doc_len_dir_len), 1, bin_file.get());
    fwrite(doc_len_dir_str.c_str(), sizeof(char), doc_len_dir_len, bin_file.get());

    std::string doc_len_meta_dir_str = doc_len_meta_dir.string();
    unsigned short doc_len_meta_dir_len = doc_len_meta_dir_str.size();
    fwrite(&doc_len_meta_dir_len, sizeof(doc_len_meta_dir_len), 1, bin_file.get());
    fwrite(doc_len_meta_dir_str.c_str(), sizeof(char), doc_len_meta_dir_len, bin_file.get());

    fwrite(&k1, sizeof(k1), 1, bin_file.get());
    fwrite(&b, sizeof(b), 1, bin_file.get());
    fwrite(&avgdl, sizeof(avgdl), 1, bin_file.get());
    fwrite(&block_size, sizeof(block_size), 1, bin_file.get());
    fwrite(&split_size, sizeof(split_size), 1, bin_file.get());
}

void read_metadata(
    const fs::path& in_path,
    fs::path& posting_dir,
    fs::path& doc_len_dir,
    fs::path& doc_len_meta_dir,
    float& k1,
    float& b,
    float& avgdl,
    int& block_size,
    size_t& split_size
) {
    SafeFile in_file(in_path, "rb");

    unsigned short posting_dir_len;
    in_file.fread(&posting_dir_len, sizeof(posting_dir_len), 1);
    std::string posting_dir_str(posting_dir_len, '\0');
    in_file.fread(&posting_dir_str[0], sizeof(char), posting_dir_len);
    posting_dir = posting_dir_str;

    unsigned short doc_len_dir_len;
    in_file.fread(&doc_len_dir_len, sizeof(doc_len_dir_len), 1);
    std::string doc_len_dir_str(doc_len_dir_len, '\0');
    in_file.fread(&doc_len_dir_str[0], sizeof(char), doc_len_dir_len);
    doc_len_dir = doc_len_dir_str;

    unsigned short doc_len_meta_dir_len;
    in_file.fread(&doc_len_meta_dir_len, sizeof(doc_len_meta_dir_len), 1);
    std::string doc_len_meta_dir_str(doc_len_meta_dir_len, '\0');
    in_file.fread(&doc_len_meta_dir_str[0], sizeof(char), doc_len_meta_dir_len);
    doc_len_meta_dir = doc_len_meta_dir_str;

    // Potentially add validations here.
    in_file.fread(&k1, sizeof(k1), 1);
    in_file.fread(&b, sizeof(b), 1);
    in_file.fread(&avgdl, sizeof(avgdl), 1);
    in_file.fread(&block_size, sizeof(block_size), 1);
    in_file.fread(&split_size, sizeof(split_size), 1);
}

void merge_inverted_blocks(
    const fs::path& in_dir,
    const fs::path& out_dir,
    const float k1,
    const float b,
    const int block_size,
    const size_t split_size
) {
    Logger logger(__FILE_NAME__, Logger::INFO);

    std::vector<unsigned int> doc_len_list;

    fs::path doc_len_dir = out_dir / file_names::DOC_LEN_LIST;
    fs::path doc_len_meta_dir = out_dir / file_names::DOC_LEN_META;

    read_doc_len_list(doc_len_dir, doc_len_list);

    // The single avgdl for this index: total tokens over documents that
    // produced at least one token. doc_len_list.size() is not used here -
    // it also counts doc_id gaps (documents with no tokens). Written to
    // metadata below, so the query engine scores with exactly the value
    // these block upper bounds were built with.
    const auto [total_docs, total_frequency] = read_doc_len_meta(doc_len_meta_dir);
    const float avgdl = static_cast<float>(
        static_cast<double>(total_frequency) / static_cast<double>(total_docs)
    );
    unsigned long long N = doc_len_list.size();

    std::vector<fs::path> in_paths = glob_files(in_dir, "", file_names::PARTIAL_BLOCK_EXT);
    sort(in_paths.begin(), in_paths.end());


    std::vector<Stream> streams;
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
    out_file.emplace(
        (out_dir / file_names::posting_file_name(file_index)),
        BUF_SIZE
    );

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
            block_size,
            avgdl,
            k1,
            b,
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

        if (cur_disk_usage && cur_disk_usage + disk_required > split_size) {
            ++file_index;
            cur_disk_usage = 0;
            out_file.emplace(
                (out_dir / file_names::posting_file_name(file_index)),
                BUF_SIZE
            );
        }

        cur_disk_usage += disk_required;
    }

    write_block_meta_file(out_dir / file_names::BLOCK_META, term_meta_mapping);

    logger.log("Finished merging inverted index blocks. Writing metadata...");

    write_metadata(
        out_dir / file_names::METADATA_TXT,
        out_dir,
        doc_len_dir,
        doc_len_meta_dir,
        k1,
        b,
        avgdl,
        block_size,
        split_size
    );

    logger.log("Finished writing metadata.");
}

IndexMeta load_index(const fs::path& meta_path) {
    IndexMeta index;

    // Stored paths are consumed but unused - see load_index in the header.
    fs::path stored_posting_dir, stored_doc_len_path, stored_doc_len_meta_path;
    read_metadata(
        meta_path,
        stored_posting_dir, stored_doc_len_path, stored_doc_len_meta_path,
        index.k1, index.b, index.avgdl, index.block_size, index.split_size
    );

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