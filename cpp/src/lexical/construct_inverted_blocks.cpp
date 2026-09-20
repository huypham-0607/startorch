#include "startorch/lexical/construct_inverted_blocks.h"
#include "startorch/lexical/file_names.h"
#include "startorch/lexical/posting_list.h"
#include "startorch/lexical/token_stream.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"
#include "startorch/utils/logger.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

constexpr unsigned int EST_UMAP_MEM_PER_ENTRY = 48; // Safe estimation of std::unordered_map mem usage per entry

namespace {
    // One body for both reader types: the SafeFile overload stays for tests.
    template <typename Reader>
    bool build_partial_index_impl(
        Reader& token_stream,
        const size_t mem_limit,
        std::unordered_map<std::string, PostingList> &posting_list_mapping,
        std::vector<std::string> &dictionary,
        DocLenCounter* const doc_lens
    ) {
        size_t mem_usage = 0;

        unsigned long long cur_doc_id;
        std::string cur_term;

        while ((mem_usage < mem_limit/5*4) && read_token(token_stream, &cur_doc_id, &cur_term)) {
            if (doc_lens != nullptr) doc_lens->add(cur_doc_id);

            // One hash lookup per token: try_emplace finds or inserts.
            auto [it, inserted] = posting_list_mapping.try_emplace(cur_term);
            if (inserted) {
                dictionary.push_back(cur_term);

                // Estimate memory usage
                mem_usage += sizeof(cur_term) + cur_term.size();
                mem_usage += sizeof(cur_term) + cur_term.size() + sizeof(PostingList) + EST_UMAP_MEM_PER_ENTRY;
            }
            if (!it->second.has_document(cur_doc_id)) {
                mem_usage += sizeof(PostingItem);
            }
            it->second.add_document(cur_doc_id);
        }

        std::sort(dictionary.begin(), dictionary.end());
        return (dictionary.size() != 0);
    }
}

bool build_partial_index(
    const SafeFile& token_stream,
    const size_t mem_limit,
    std::unordered_map<std::string, PostingList> &posting_list_mapping,
    std::vector<std::string> &dictionary
) {
    return build_partial_index_impl(token_stream, mem_limit, posting_list_mapping, dictionary, nullptr);
}

bool build_partial_index(
    BufferedReader& token_stream,
    const size_t mem_limit,
    std::unordered_map<std::string, PostingList> &posting_list_mapping,
    std::vector<std::string> &dictionary,
    DocLenCounter* const doc_lens
) {
    return build_partial_index_impl(token_stream, mem_limit, posting_list_mapping, dictionary, doc_lens);
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
    BufferedWriter out_file(out_file_path);

    unsigned int dictionary_size = dictionary.size();
    out_file.write(dictionary_size);

    unsigned char vbe_buffer[8];
    for (const std::string& term : dictionary) {
        PostingList& postings = posting_list_mapping.at(term);

        // The token stream is not in doc_id order, and the gaps below must
        // not underflow. Sorting can also shrink the list, so do it first.
        postings.sort();

        unsigned short term_size = term.size();                         // 2 bytes
        unsigned int posting_list_size = postings.size();               // 4 bytes should be sufficient

        out_file.write(term_size);
        out_file.write(posting_list_size);

        out_file.fwrite(term.c_str(), sizeof(char), term.size());

        // VBE encoding
        unsigned long long last = 0;
        for (size_t i = 0; i < postings.size(); i++) {
            const PostingItem& item = postings[i];
            unsigned long long delta = item.doc_id - last;
            int encode_length = vbe_encode(delta, vbe_buffer);

            out_file.fwrite(vbe_buffer, sizeof(unsigned char), encode_length);
            out_file.write(item.freq);
            last = item.doc_id;
        }
    }
}

void construct_inverted_blocks(
    const fs::path& in_dir,
    const fs::path& out_dir,
    const size_t mem_limit,
    DocLenCounter* const doc_lens
) {
    Logger logger(__FILE_NAME__, Logger::INFO);
    std::unordered_map<std::string, PostingList> posting_list_mapping;
    std::vector<std::string> dictionary;

    std::vector<fs::path> token_streams = glob_files(in_dir, "", file_names::PARTIAL_BLOCK_EXT);
    sort(token_streams.begin(), token_streams.end());

    logger.log("Started building inverted index blocks.");

    int partial_block_counter = 0;

    for (auto &token_stream : token_streams) {
        BufferedReader in(token_stream);

        while (build_partial_index(
            in,
            mem_limit,
            posting_list_mapping,
            dictionary,
            doc_lens
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
