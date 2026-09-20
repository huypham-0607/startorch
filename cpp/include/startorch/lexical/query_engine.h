#ifndef QUERY_ENGINE_H
#define QUERY_ENGINE_H

#include "startorch/lexical/merge_inverted_blocks.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/logger.h"

#include <chrono>
#include <format>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

// A SafeMMAP class here for deep-pointer movement.

const unsigned long long MAX_DOC_ID = std::numeric_limits<unsigned long long>::max();

class PostingPointer {
public:
    PostingPointer(
        const std::string& _term,
        const int _block_size,
        std::unordered_map<std::string, TermMeta> &term_meta_mapping,
        std::unordered_map<unsigned int, SafeFileMmap> &file_index_mapping
    );

    int get_cur_block_size(const int block_id) const;
    unsigned int get_doc_count() const;
    unsigned long long get_doc_id() const;
    unsigned long long get_shallow_block_id() const;
    unsigned long long get_block_count() const;
    unsigned long long get_next_block_doc_id() const;
    float get_term_upper_bound() const;
    float get_block_upper_bound() const;
    unsigned int get_frequency() const;

    void next_shallow(const unsigned long long target_doc_id);
    void next_shallow_deep(const unsigned long long target_doc_id);
    void next(const unsigned long long target_doc_id);
    
private:
    std::string term;
    TermMeta* term_meta;
    SafeFileMmap* posting_file;
    int block_size;
    int cur_block_id;
    int deep_block_id;
    size_t cur_addr;
    unsigned long long doc_id;
    int doc_pos;

    size_t read_posting_entry(unsigned long long &delta, unsigned int &freq) const;
    int find_block(const unsigned long long target_doc_id) const;
};

void sort_posting(std::vector<PostingPointer>& postings);

int find_pivot(std::vector<PostingPointer>& postings, const float theta);

bool check_block_max(std::vector<PostingPointer>& postings, const int pivot, const float theta);

float evaluate_prefix(
    std::vector<PostingPointer>& postings,
    const int pivot,
    std::vector<unsigned int>& doc_len_list,
    const float avgdl,
    const float k1,
    const float b
);

void advance_prefix(
    std::vector<PostingPointer>& postings,
    const int pivot,
    const unsigned long long target_doc_id
);

/**
 * @brief Advance one cursor to target_doc_id: among the sorted prefix whose
 * doc id satisfies cmp(doc_id, pivot doc), the one with the smallest df
 * (highest IDF). The only difference between call sites is cmp:
 * std::less<>{} considers cursors strictly before the pivot doc,
 * std::less_equal<>{} also includes the pivot doc's tied run.
 */
template <typename Compare>
void advance_one(
    std::vector<PostingPointer>& postings,
    const int pivot,
    const unsigned long long N,
    const unsigned long long target_doc_id,
    Compare cmp
) {
    unsigned long long doc = postings[pivot].get_doc_id();

    unsigned int min_df = N+1;
    int advance_id = -1;

    int idx = 0;
    while (idx < static_cast<int>(postings.size()) && cmp(postings[idx].get_doc_id(), doc)) {

        const unsigned int df = postings[idx].get_doc_count();
        if (df < min_df) {
            min_df = df;
            advance_id = idx;
        }

        ++idx;
    }
    if (advance_id == -1) {
        throw std::runtime_error(std::format(
            "Unable to find advancing index for pivot doc_id {}.",
            doc
        ));
    }
    postings[advance_id].next(target_doc_id);
}

unsigned long long get_new_candidate(
    std::vector<PostingPointer>& postings,
    const int pivot
);

// Best-first (score, doc_id) pairs.
using QueryResult = std::vector<std::pair<float, unsigned long long>>;
using QueryElapsed = std::chrono::duration<double, std::milli>;

/**
 * @brief An opened index that answers many queries.
 *
 * The constructor does the whole index load once (metadata and block
 * metadata via load_index, doc lengths, posting-file mmaps). Callers keep one
 * engine and call query()/query_exhaustive() repeatedly; each call returns its
 * results and the time spent searching, which excludes the index load.
 */
class QueryEngine {
public:
    explicit QueryEngine(const fs::path& meta_path);

    // Block-Max WAND top-k.
    std::pair<QueryResult, QueryElapsed> query(
        const std::vector<std::string>& raw_terms,
        const int k
    );

    // Scores every candidate with no pruning. Ground truth for query(): the
    // results must match exactly, only the time differs.
    std::pair<QueryResult, QueryElapsed> query_exhaustive(
        const std::vector<std::string>& raw_terms,
        const int k
    );

private:
    using TopK = std::priority_queue<
        std::pair<float, unsigned long long>,
        std::vector<std::pair<float, unsigned long long>>,
        std::greater<std::pair<float, unsigned long long>>
    >;

    QueryResult search(const std::vector<std::string>& raw_terms, const int k);
    QueryResult search_exhaustive(const std::vector<std::string>& raw_terms, const int k);

    std::vector<PostingPointer> open_postings(const std::vector<std::string>& raw_terms);
    static TopK make_top_k(const int k);
    static QueryResult drain_top_k(TopK& top_k);

    Logger logger;
    fs::path in_path;
    float k1, b;
    int block_size;
    size_t split_size;
    std::vector<unsigned int> doc_len_list;
    float avgdl;
    std::unordered_map<std::string, TermMeta> term_meta_mapping;
    std::unordered_map<unsigned int, SafeFileMmap> file_index_mapping;
};

#endif