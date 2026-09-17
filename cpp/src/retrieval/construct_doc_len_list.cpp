#include "startorch/retrieval/construct_doc_len_list.h"
#include "startorch/retrieval/file_names.h"
#include "startorch/retrieval/token_stream.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"
#include "startorch/utils/logger.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <stdexcept>

namespace fs = std::filesystem;

constexpr size_t BUF_SIZE = (1<<20);

void write_doc_len_entry(
    BufferedWriter& out_fp,
    const unsigned long long& delta,
    const unsigned int& freq
) {
    unsigned char buffer[8];
    size_t encode_len = vbe_encode(delta, buffer);
    out_fp.fwrite(buffer, sizeof(unsigned char), encode_len);
    out_fp.fwrite(&freq, sizeof(freq), 1);
}

void construct_doc_len_list(
    const fs::path& in_dir,
    const fs::path& out_dir
) {
    Logger logger(__FILE_NAME__, Logger::INFO);
    std::vector<fs::path> token_streams = glob_files(in_dir, "", file_names::PARTIAL_BLOCK_EXT);
    sort(token_streams.begin(), token_streams.end());

    fs::path out_file_path = out_dir / file_names::DOC_LEN_LIST;

    BufferedWriter out_fp(out_file_path, BUF_SIZE);

    logger.log("Started constructing document length list.");

    // Token count per doc_id, indexed by doc_id. The token stream is not in
    // doc_id order, so lengths are counted by id rather than by runs of equal
    // ids. Mapped doc ids are dense in [0, N), so this is the same N-entry
    // array merge_inverted_blocks and the query engine load.
    std::vector<unsigned int> doc_len;
    unsigned long long total_frequency = 0;

    for (auto &token_stream : token_streams) {
        SafeFile fp(token_stream, "rb");
        unsigned long long cur_doc_id = 0;
        std::string term;

        while (read_token(fp, &cur_doc_id, &term)) {
            if (cur_doc_id >= doc_len.size()) {
                doc_len.resize(cur_doc_id + 1, 0);
            }
            ++doc_len[cur_doc_id];
            ++total_frequency;
        }
    }

    // Written in doc_id order, skipping ids with no tokens.
    unsigned long long total_docs = 0;
    unsigned long long prev_doc_id = 0;
    for (unsigned long long doc_id = 0; doc_id < doc_len.size(); doc_id++) {
        if (doc_len[doc_id] == 0) continue;
        write_doc_len_entry(out_fp, doc_id - prev_doc_id, doc_len[doc_id]);
        prev_doc_id = doc_id;
        ++total_docs;
    }

    fs::path out_meta_path = out_dir / file_names::DOC_LEN_META;

    write_doc_len_meta(out_meta_path, total_docs, total_frequency);
    logger.log("Finished constructing document length list.");
}

void write_doc_len_meta(const fs::path out_path, unsigned long long total_docs, unsigned long long total_frequency) {
    SafeFile out_file(out_path, "wb");
    out_file.fwrite(&total_docs, sizeof(total_docs), 1);
    out_file.fwrite(&total_frequency, sizeof(total_docs), 1);
}

const std::pair<unsigned long long, unsigned long long> read_doc_len_meta(const fs::path in_path) {
    SafeFile in_file(in_path, "rb");
    unsigned long long total_docs;
    unsigned long long total_frequency;
    in_file.fread(&total_docs, sizeof(total_docs), 1);
    in_file.fread(&total_frequency, sizeof(total_frequency), 1);
    return std::make_pair(total_docs, total_frequency);
}

bool read_doc_len_entry(
    const SafeFile& in_file,
    unsigned long long* const ptr_offset,
    unsigned int* const ptr_freq
) {
    size_t arg_count;
    long initial_pos = ftell(in_file.get());

    unsigned char buffer[8];
    bool res = read_vbe(in_file.get(), buffer);

    if (!res) {
        long bytes_read = ftell(in_file.get()) - initial_pos;
        if (bytes_read == 0) return false;
        throw std::runtime_error(std::format(
            "Error while reading doc_len entry."
        ));
    }
    (*ptr_offset) = vbe_decode(buffer);

    arg_count = fread(ptr_freq, sizeof(*ptr_freq), 1, in_file.get());
    if (arg_count != 1) throw std::runtime_error(std::format(
        "Error while reading doc_len entry."
    ));
    return true;
}

void read_doc_len_list(
    const fs::path& in_dir,
    std::vector<unsigned int>& doc_len_list
) {
    SafeFile in_file(in_dir, "rb");
    unsigned long long doc_id = 0;
    unsigned long long delta;
    unsigned int freq;
    while (read_doc_len_entry(in_file, &delta, &freq)) {
        doc_id += delta;
        doc_len_list.resize(std::max((size_t)doc_id + 1, doc_len_list.size()), 0);
        doc_len_list[doc_id] = freq;
    }
}
