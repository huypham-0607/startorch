#include "startorch/retrieval/construct_inverted_blocks.h"
#include "startorch/retrieval/file_names.h"
#include "startorch/retrieval/posting_list.h"
#include "startorch/retrieval/token_stream.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"
#include "startorch/utils/logger.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

constexpr unsigned int EST_UMAP_MEM_PER_ENTRY = 48; // Safe estimation of std::unordered_map mem usage per entry

constexpr size_t BUF_SIZE = (1<<20);

bool build_partial_index(
    const SafeFile& token_stream,
    const size_t mem_limit,
    std::unordered_map<std::string, PostingList> &posting_list_mapping,
    std::vector<std::string> &dictionary
) {
    size_t mem_usage = 0;

    unsigned long long cur_doc_id;
    std::string cur_term;

    while ((mem_usage < mem_limit/5*4) && read_token(token_stream, &cur_doc_id, &cur_term)) {
        if (posting_list_mapping.find(cur_term) == posting_list_mapping.end()) {
            dictionary.push_back(cur_term);
            posting_list_mapping[cur_term] = PostingList();

            // Estimate memory usage
            mem_usage += sizeof(cur_term) + cur_term.size();
            mem_usage += sizeof(cur_term) + cur_term.size() + sizeof(PostingList) + EST_UMAP_MEM_PER_ENTRY;
        }
        if (!posting_list_mapping[cur_term].has_document(cur_doc_id)) {
            mem_usage += sizeof(PostingItem);
        }
        posting_list_mapping[cur_term].add_document(cur_doc_id);
    }

    std::sort(dictionary.begin(), dictionary.end());
    return (dictionary.size() != 0);
}

/**
 * @brief Write posting list to file.
 *
 * Starts with a dictionary_size <unsigned int>
 * Format for each posting list:
 *
 *       <term_size><posting_list_size><term><<vbe_encoding_{i}><freq_{i}>>
 *
 * - term_size:             unsigned short
 * - posting_list_size:     unsigned int
 * - term:                  char[]
 * - vbe_encoding_{i}:      unsigned char[]
 * - freq:                  unsigned int
 */
void write_partial_index(
    const fs::path out_file_path,
    std::unordered_map<std::string, PostingList>& posting_list_mapping,
    std::vector<std::string>& dictionary
) {
    BufferedWriter out_file(out_file_path, BUF_SIZE);

    unsigned int dictionary_size = dictionary.size();
    out_file.fwrite(&dictionary_size, sizeof(dictionary_size), 1);

    unsigned char vbe_buffer[8];
    for (std::string term : dictionary) {
        // The token stream is not in doc_id order, and the gaps below must
        // not underflow. Sorting can also shrink the list, so do it first.
        posting_list_mapping[term].sort();

        unsigned short term_size = term.size();                                 // 2 bytes
        unsigned int posting_list_size = posting_list_mapping[term].size();     // 4 bytes should be sufficient

        out_file.fwrite(&term_size, sizeof(term_size), 1);
        out_file.fwrite(&posting_list_size, sizeof(posting_list_size), 1);

        out_file.fwrite(term.c_str(), sizeof(char), term.size());

        // VBE encoding
        unsigned long long last = 0;
        for (int i = 0; i < posting_list_mapping[term].size(); i++) {
            PostingItem item = posting_list_mapping[term][i];
            unsigned long long delta = item.doc_id - last;
            int encode_length = vbe_encode(delta, vbe_buffer);

            out_file.fwrite(vbe_buffer, sizeof(unsigned char), encode_length);
            out_file.fwrite(&item.freq, sizeof(item.freq), 1);
            last = item.doc_id;
        }
    }
}

void construct_inverted_blocks(
    const fs::path& in_dir,
    const fs::path& out_dir,
    const size_t mem_limit
) {
    Logger logger(__FILE_NAME__, Logger::INFO);
    std::unordered_map<std::string, PostingList> posting_list_mapping;
    std::vector<std::string> dictionary;

    std::vector<fs::path> token_streams = glob_files(in_dir, "", file_names::PARTIAL_BLOCK_EXT);
    sort(token_streams.begin(), token_streams.end());

    logger.log("Started building inverted index blocks.");

    int partial_block_counter = 0;

    for (auto &token_stream : token_streams) {
        SafeFile fp(token_stream, "rb");

        while (build_partial_index(
            fp,
            mem_limit,
            posting_list_mapping,
            dictionary
        )) {
            write_partial_index(
                out_dir / file_names::partial_block_file_name(partial_block_counter),
                posting_list_mapping,
                dictionary
            );

            ++partial_block_counter;
            posting_list_mapping.clear();
            dictionary.clear();
        }
    }

    logger.log("Finished building inverted index blocks.");
}
