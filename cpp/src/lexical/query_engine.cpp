/**
 * @file query_engine.cpp
 * 
 * @brief Query engine for Lexical retrieval
 * 
 */

#include "startorch/lexical/query_engine.h"
#include "startorch/lexical/bm25.h"
#include "startorch/lexical/file_names.h"
#include "startorch/lexical/merge_inverted_blocks.h"
#include "startorch/lexical/construct_doc_len_list.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"
#include "startorch/utils/logger.h"

#include <vector>
#include <format>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <bit>
#include <functional>
#include <queue>
#include <chrono>
#include <cstring>

namespace fs = std::filesystem;


PostingPointer::PostingPointer(
    const std::string& _term,
    const int _block_size,
    const std::unordered_map<std::string, TermMeta> &term_meta_mapping,
    const std::unordered_map<unsigned int, SafeFileMmap> &file_index_mapping
) : term(_term) {
    // find() is a read under the standard's container data-race rules;
    // operator[] is not, even when the key exists, so it must not be used here.
    const auto term_it = term_meta_mapping.find(term);
    if (term_it == term_meta_mapping.end()) {
        throw std::runtime_error(std::format(
            "Unable to initialize PostingPointer: Term {} not found in term_meta_mapping",
            term
        ));
    }
    term_meta = &term_it->second;
    auto file_it = file_index_mapping.find(term_meta->file_index);
    if (file_it == file_index_mapping.end()) {
        throw std::runtime_error(std::format(
            "Unable to initialize PostingPointer: file_index {} not found in file_index_mapping",
            term_meta->file_index
        ));    
    }
    block_size = _block_size;
    posting_file = &(file_it->second);
    cur_block_id = 0;
    deep_block_id = 0;
    cur_addr = term_meta->block_meta_list[0].start_addr;
    doc_id = term_meta->block_meta_list[0].doc_id;
    doc_pos = 0;
}

int PostingPointer::get_cur_block_size(const int block_id) const {
    if (block_id == static_cast<int>(term_meta->block_meta_list.size()) - 1) {
        return term_meta->doc_count - block_size * block_id;
    }
    return block_size;
}

unsigned int PostingPointer::get_doc_count() const {
    return term_meta->doc_count;
}

unsigned long long PostingPointer::get_doc_id() const {
    return doc_id;
}

unsigned long long PostingPointer::get_shallow_block_id() const {
    return cur_block_id;
}

unsigned long long PostingPointer::get_block_count() const {
    return term_meta->block_meta_list.size();
}

unsigned long long PostingPointer::get_next_block_doc_id() const {
    if (cur_block_id + 1 >= static_cast<int>(term_meta->block_meta_list.size())) {
        return MAX_DOC_ID;
    }
    else {
        return term_meta->block_meta_list[cur_block_id + 1].doc_id;
    }
}

float PostingPointer::get_term_upper_bound() const {
    return term_meta->term_ub;
}

float PostingPointer::get_block_upper_bound() const {
    if (cur_block_id == static_cast<int>(term_meta->block_meta_list.size())) {
        throw std::runtime_error(std::format(
            "Unable to get block upper_bound: cur_block_id {} exceeded range [0,{}).",
            cur_block_id, term_meta->block_meta_list.size()
        ));
    }
    return term_meta->block_meta_list[cur_block_id].block_ub;
}

unsigned int PostingPointer::get_frequency() const {
    if (doc_id == MAX_DOC_ID) {
        throw std::runtime_error(std::format(
            "Unable to get entry frequency for doc_id {}.",
            doc_id
        ));
    }
    unsigned long long delta; unsigned int freq;
    read_posting_entry(delta, freq);
    return freq;
}

// Index of the block that would contain target_doc_id.
int PostingPointer::find_block(const unsigned long long target_doc_id) const {
    auto it = std::upper_bound(
        term_meta->block_meta_list.begin(),
        term_meta->block_meta_list.end(),
        target_doc_id,
        [&] (unsigned long long val, BlockMeta x) {
            return val < x.doc_id;
        }
    );
    int new_block = static_cast<int>((it - term_meta->block_meta_list.begin()) - 1);

    // Impossible given how BMW works.
    if (new_block < cur_block_id) {
        throw std::runtime_error(std::format(
            "Unable to advance PostingPointer: new_block id {} is less than cur_block id {}.",
            new_block, cur_block_id
        ));
    }

    return new_block;
}

void PostingPointer::next_shallow(const unsigned long long target_doc_id) {
    cur_block_id = find_block(target_doc_id);
}

void PostingPointer::next_shallow_deep(const unsigned long long target_doc_id) {
    int new_block = find_block(target_doc_id);

    if (new_block > deep_block_id) {
        deep_block_id = new_block;
        cur_block_id = new_block;
        cur_addr = term_meta->block_meta_list[new_block].start_addr;
        doc_id = term_meta->block_meta_list[new_block].doc_id;
        doc_pos = 0;
    }
}

void PostingPointer::next(const unsigned long long target_doc_id) {
    next_shallow_deep(target_doc_id);

    if (doc_id >= target_doc_id) {
        return;
    }
    
    unsigned long long delta;
    unsigned int freq;
    cur_addr += read_posting_entry(delta, freq);
    for (int i = doc_pos+1; i < get_cur_block_size(deep_block_id); i++){
        size_t entry_size = read_posting_entry(delta, freq);
        doc_id += delta;
        ++doc_pos;

        if (doc_id >= target_doc_id) break;

        cur_addr += entry_size;
    }

    if (doc_id < target_doc_id) {
        ++deep_block_id;
        ++cur_block_id;
        if (deep_block_id == static_cast<int>(term_meta->block_meta_list.size())) {
            cur_addr = term_meta->end_addr;
            doc_id = MAX_DOC_ID;
            doc_pos = 0;
        }
        else {
            cur_addr = term_meta->block_meta_list[deep_block_id].start_addr;
            doc_id = term_meta->block_meta_list[deep_block_id].doc_id;
            doc_pos = 0;
        }
    }
}


// Return size of the entry (for posting iteration)
size_t PostingPointer::read_posting_entry(
    unsigned long long &delta,
    unsigned int &freq
) const {
    const size_t file_size = posting_file->size();
    if (cur_addr >= file_size) {
        throw std::runtime_error(std::format(
            "Unable to read VBE Encoding in posting {}: address {} is past the end of the file.",
            term, cur_addr
        ));
    }
    const unsigned char* entry = posting_file->data() + cur_addr;
    const size_t available = file_size - cur_addr;

    // Throws on a value with no terminator within BUFFER_LIMIT bytes.
    const size_t vbe_size = vbe_decode_from(entry, available, delta);
    if (vbe_size == 0 || available - vbe_size < sizeof(freq)) {
        throw std::runtime_error(std::format(
            "Unable to read VBE Encoding in posting {}.",
            term
        ));
    }

    // freq was written in native byte order (BufferedWriter::fwrite).
    std::memcpy(&freq, entry + vbe_size, sizeof(freq));
    return vbe_size + sizeof(freq);
}

void sort_posting(std::vector<PostingPointer>& postings) {
    std::sort(postings.begin(), postings.end(), [&](const PostingPointer& a, const PostingPointer& b) {
        return a.get_doc_id() < b.get_doc_id();
    });
}

int find_pivot(std::vector<PostingPointer>& postings, const float theta) {
    float running_score = 0.0;
    for (int i = 0; i < static_cast<int>(postings.size()); i++) {
        running_score += postings[i].get_term_upper_bound();
        if (running_score > theta) {
            return i;
        }
    }
    return static_cast<int>(postings.size());

}

bool check_block_max(std::vector<PostingPointer>& postings, const int pivot, const float theta) {
    unsigned long long doc = postings[pivot].get_doc_id();
    
    float running_sum = 0.0;
    int idx = 0;
    while (idx < static_cast<int>(postings.size()) && postings[idx].get_doc_id() <= doc) {
        running_sum += postings[idx].get_block_upper_bound();
        ++idx;
    }
    return (running_sum > theta);
}

float evaluate_prefix(
    std::vector<PostingPointer>& postings,
    const int pivot,
    const std::vector<unsigned int>& doc_len_list,
    const float avgdl,
    const float k1,
    const float b
) {
    float running_sum = 0.0;
    unsigned long long doc = postings[pivot].get_doc_id();
    int idx = 0;
    while (idx <static_cast<int>(postings.size()) && postings[idx].get_doc_id() <= doc) {
        // Prefix of postings has to be tied run of doc_id = doc
        if (postings[idx].get_doc_id() != doc) {
            throw std::runtime_error(std::format(
                "Unable to complete full evaluation for doc_id {}: postings[{}].get_doc_id() expected {}, found {}.",
                doc, idx, doc, postings[idx].get_doc_id()
            ));
        }

        const unsigned long long N = doc_len_list.size();
        const unsigned int df_t = postings[idx].get_doc_count();
        const unsigned int tf = postings[idx].get_frequency();
        const float doc_len = doc_len_list[postings[idx].get_doc_id()];

        running_sum += calc_BM25(N, df_t, tf, doc_len, avgdl, k1, b);
        ++idx;
    }
    return running_sum;
}

void advance_prefix(
    std::vector<PostingPointer>& postings,
    const int pivot,
    const unsigned long long target_doc_id
) {
    unsigned long long doc = postings[pivot].get_doc_id();
    int idx = 0;
    while (idx < static_cast<int>(postings.size()) && postings[idx].get_doc_id() <= doc) {
        // Prefix of postings has to be tied run of doc_id = doc
        if (postings[idx].get_doc_id() != doc) {
            throw std::runtime_error(std::format(
                "Unable to complete full evaluation for doc_id {}: postings[{}].get_doc_id() expected {}, found {}.",
                doc, idx, doc, postings[idx].get_doc_id()
            ));
        }

        postings[idx].next(target_doc_id);

        ++idx;
    }
}

unsigned long long get_new_candidate(
    std::vector<PostingPointer>& postings,
    const int pivot
) {
    unsigned long long doc = postings[pivot].get_doc_id();

    unsigned long long target_doc_id = MAX_DOC_ID;
    int idx = 0;
    while (idx < static_cast<int>(postings.size()) && postings[idx].get_doc_id() <= doc) {
        unsigned long long cand = postings[idx].get_next_block_doc_id();
        target_doc_id = std::min(target_doc_id, cand);
        ++idx;
    }

    if (idx < static_cast<int>(postings.size())) target_doc_id = std::min(target_doc_id, postings[idx].get_doc_id());

    return target_doc_id;
}

QueryEngine::QueryEngine(const fs::path& meta_path) : logger(__FILE_NAME__, Logger::INFO) {
    logger.log("Initializing QueryEngine...");
    logger.log("Loading metadata and term_meta_mapping...");

    // avgdl comes from metadata, not recomputed: it must be exactly the
    // value merge_inverted_blocks built the block upper bounds with, or
    // those bounds can underestimate real scores.
    IndexMeta index = load_index(meta_path);
    in_path = index.posting_dir;
    k1 = index.k1;
    b = index.b;
    avgdl = index.avgdl;
    block_size = index.block_size;
    split_size = index.split_size;

    logger.log("Finished loading metadata and term_meta_mapping, loading doc_len_list...");

    read_doc_len_list(in_path / file_names::DOC_LEN_LIST, doc_len_list);

    logger.log("Finished loading doc_len_list. Opening posting files...");

    for (auto& [term, metadata] : index.terms) {
        unsigned int file_index = metadata.file_index;
        term_meta_mapping.emplace(std::move(term), std::move(metadata));

        if (file_index_mapping.find(file_index) == file_index_mapping.end()) {
            file_index_mapping.emplace(
                file_index,
                in_path / file_names::posting_file_name(file_index)
            );
        }
    }
    logger.log("Finished opening posting files. QueryEngine ready.");
}

std::pair<QueryResult, QueryElapsed> QueryEngine::query(
    const std::vector<std::string>& raw_terms,
    const int k
) const {
    auto start = std::chrono::high_resolution_clock::now();
    QueryResult result = search(raw_terms, k);
    QueryElapsed elapsed = std::chrono::high_resolution_clock::now() - start;

    logger.log(std::format("Finished query. Time elapsed: {}", elapsed));
    return std::make_pair(std::move(result), elapsed);
}

std::pair<QueryResult, QueryElapsed> QueryEngine::query_exhaustive(
    const std::vector<std::string>& raw_terms,
    const int k
) const {
    auto start = std::chrono::high_resolution_clock::now();
    QueryResult result = search_exhaustive(raw_terms, k);
    QueryElapsed elapsed = std::chrono::high_resolution_clock::now() - start;

    logger.log(std::format("Finished exhaustive query. Time elapsed: {}", elapsed));
    return std::make_pair(std::move(result), elapsed);
}

QueryResult QueryEngine::search(
    const std::vector<std::string>& raw_terms,
    const int k
) const {
    std::vector<PostingPointer> postings = open_postings(raw_terms);
    TopK top_k = make_top_k(k);

    for (unsigned long long epoch = 0; epoch < (1LL<<32); epoch++) {
        sort_posting(postings);

        float theta = top_k.top().first;
        int pivot = find_pivot(postings, theta);

        if (pivot == static_cast<int>(postings.size())) break;
        unsigned long long doc = postings[pivot].get_doc_id();
        if (doc == MAX_DOC_ID) break;

        for (int idx = 0; idx < pivot; idx++) {
            postings[idx].next_shallow(doc);
        }

        bool flag = check_block_max(postings, pivot, theta);
        if (flag) {
            if (postings[0].get_doc_id() == doc) {
                float score = evaluate_prefix(
                    postings,
                    pivot,
                    doc_len_list,
                    avgdl,
                    k1,
                    b
                );
                if (score > theta) {
                    top_k.pop();
                    top_k.push(std::make_pair(score, doc));
                }
                advance_prefix(
                    postings,
                    pivot,
                    doc + 1
                );
            }
            else {
                // std::less: only cursors lagging behind the pivot doc move, up to doc itself.
                advance_one(postings, pivot, doc_len_list.size(), doc, std::less<>{});
            }
        }
        else {
            unsigned long long target_doc_id = get_new_candidate(postings, pivot);
            // std::less_equal: no doc before target_doc_id can beat theta, so the pivot's run may skip too.
            advance_one(postings, pivot, doc_len_list.size(), target_doc_id, std::less_equal<>{});
        }
    }
    return drain_top_k(top_k);
}

QueryResult QueryEngine::search_exhaustive(
    const std::vector<std::string>& raw_terms,
    const int k
) const {
    std::vector<PostingPointer> postings = open_postings(raw_terms);
    TopK top_k = make_top_k(k);

    for (unsigned long long epoch = 0; epoch < (1LL<<32); epoch++) {
        sort_posting(postings);

        if (postings.empty() || postings.front().get_doc_id() == MAX_DOC_ID) break;
        unsigned long long doc = postings.front().get_doc_id();

        float score = evaluate_prefix(
            postings,
            0,
            doc_len_list,
            avgdl,
            k1,
            b
        );

        if (score > top_k.top().first) {
            top_k.pop();
            top_k.push(std::make_pair(score, doc));
        }

        advance_prefix(postings, 0, doc + 1);
    }
    return drain_top_k(top_k);
}

// One cursor per query term; terms not in the index are dropped.
std::vector<PostingPointer> QueryEngine::open_postings(const std::vector<std::string>& raw_terms) const {
    std::vector<PostingPointer> postings;
    for (const auto& term : raw_terms) {
        if (term_meta_mapping.find(term) != term_meta_mapping.end()) {
            postings.push_back(PostingPointer(
                term, block_size, term_meta_mapping, file_index_mapping
            ));
        }
    }
    return postings;
}

// Min-heap seeded with k sentinels (score -1), so top() is theta from the start.
QueryEngine::TopK QueryEngine::make_top_k(const int k) {
    TopK top_k;
    for (int i = 0; i < k; i++) top_k.push(std::make_pair(-1, MAX_DOC_ID));
    return top_k;
}

// Best-first results with the unfilled sentinels removed.
QueryResult QueryEngine::drain_top_k(TopK& top_k) {
    QueryResult res;
    while (!top_k.empty()) {
        if (top_k.top().first != -1) res.push_back(top_k.top());
        top_k.pop();
    }
    std::reverse(res.begin(), res.end());
    return res;
}
