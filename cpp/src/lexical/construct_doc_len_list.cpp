#include "startorch/lexical/construct_doc_len_list.h"
#include "startorch/lexical/file_names.h"
#include "startorch/lexical/token_stream.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"
#include "startorch/utils/logger.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <stdexcept>

namespace fs = std::filesystem;

void write_doc_len_entry(
    BufferedWriter& out_fp,
    const unsigned long long& delta,
    const unsigned int& freq
) {
    unsigned char buffer[8];
    size_t encode_len = vbe_encode(delta, buffer);
    out_fp.fwrite(buffer, sizeof(unsigned char), encode_len);
    out_fp.write(freq);
}

unsigned long long DocLenCounter::total_docs() const {
    return static_cast<unsigned long long>(
        std::count_if(lengths.begin(), lengths.end(), [](unsigned int len) { return len != 0; })
    );
}

void DocLenCounter::write(const fs::path& out_dir) const {
    BufferedWriter out_fp(out_dir / file_names::DOC_LEN_LIST);

    // Written in doc_id order, skipping ids with no tokens.
    unsigned long long docs = 0;
    unsigned long long prev_doc_id = 0;
    for (unsigned long long doc_id = 0; doc_id < lengths.size(); doc_id++) {
        if (lengths[doc_id] == 0) continue;
        write_doc_len_entry(out_fp, doc_id - prev_doc_id, lengths[doc_id]);
        prev_doc_id = doc_id;
        ++docs;
    }

    write_doc_len_meta(out_dir / file_names::DOC_LEN_META, docs, total_frequency);
}

void construct_doc_len_list(
    const fs::path& in_dir,
    const fs::path& out_dir
) {
    Logger logger(__FILE_NAME__, Logger::INFO);
    // Fail before the pass over the token stream, not after it.
    if (!fs::is_directory(out_dir)) {
        throw std::runtime_error(std::format("Output directory {} does not exist.", out_dir.string()));
    }
    std::vector<fs::path> token_streams = glob_files(in_dir, "", file_names::PARTIAL_BLOCK_EXT);
    sort(token_streams.begin(), token_streams.end());

    logger.log("Started constructing document length list.");

    DocLenCounter counter;
    for (auto &token_stream : token_streams) {
        BufferedReader in(token_stream);
        unsigned long long cur_doc_id = 0;
        std::string term;

        while (read_token(in, &cur_doc_id, &term)) {
            counter.add(cur_doc_id);
        }
    }

    counter.write(out_dir);
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
    BufferedReader in_file(in_dir);
    unsigned long long doc_id = 0;
    unsigned long long delta;
    unsigned int freq;
    while (in_file.read_vbe(delta)) {
        if (!in_file.read(freq)) throw std::runtime_error(std::format(
            "Error while reading doc_len entry."
        ));
        doc_id += delta;
        if (doc_id >= doc_len_list.size()) doc_len_list.resize(doc_id + 1, 0);
        doc_len_list[doc_id] = freq;
    }
}
