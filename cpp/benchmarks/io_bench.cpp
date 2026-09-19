/**
 * @file io_bench.cpp
 * @brief Read-path micro-benchmark: the legacy stdio readers against
 * BufferedReader, over the same file. Read-only.
 *
 *   io_bench tokens    <token_*.bin>        read_token(SafeFile) vs read_token(BufferedReader)
 *   io_bench partial   <block_*.bin>        read_vbe(FILE*) + fread vs BufferedReader, as the merge reads
 *   io_bench doclen    <doc_len_list.bin>   read_doc_len_entry(SafeFile) loop vs read_doc_len_list
 *   io_bench blockmeta <block_meta.bin>     read_block_meta_file
 *
 * Warm the page cache first (e.g. cat file > /dev/null) to measure CPU cost
 * rather than the drive.
 */

#include "startorch/retrieval/construct_doc_len_list.h"
#include "startorch/retrieval/merge_inverted_blocks.h"
#include "startorch/retrieval/token_stream.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"

#include <chrono>
#include <cstdio>
#include <format>
#include <iostream>
#include <string>
#include <vector>

// Legacy reader, defined in construct_doc_len_list.cpp and kept for tests.
bool read_doc_len_entry(const SafeFile& in_file, unsigned long long* const ptr_offset, unsigned int* const ptr_freq);

namespace {
    using clock_type = std::chrono::steady_clock;

    double seconds_since(clock_type::time_point start) {
        return std::chrono::duration<double>(clock_type::now() - start).count();
    }

    void report(const std::string& mode, unsigned long long records, double legacy_s, double buffered_s) {
        std::cout << std::format(
            "{:<9} records={} legacy={:.2f}s buffered={:.2f}s ({:.0f} vs {:.0f} ns/record)\n",
            mode, records, legacy_s, buffered_s,
            legacy_s / records * 1e9, buffered_s / records * 1e9
        );
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <tokens|partial|doclen|blockmeta> <file>\n";
        return 1;
    }
    const std::string mode = argv[1];
    const fs::path path = argv[2];

    if (mode == "tokens") {
        unsigned long long legacy_n = 0, buffered_n = 0, doc_id;
        std::string term;

        auto start = clock_type::now();
        {
            SafeFile in(path, "rb");
            while (read_token(in, &doc_id, &term)) ++legacy_n;
        }
        const double legacy_s = seconds_since(start);

        start = clock_type::now();
        {
            BufferedReader in(path);
            while (read_token(in, &doc_id, &term)) ++buffered_n;
        }
        const double buffered_s = seconds_since(start);

        if (legacy_n != buffered_n) std::cerr << "record count mismatch\n";
        report(mode, buffered_n, legacy_s, buffered_s);
    }
    else if (mode == "partial") {
        unsigned long long legacy_n = 0, buffered_n = 0;

        auto start = clock_type::now();
        {
            SafeFile in(path, "rb");
            unsigned int dict_size;
            in.fread(&dict_size, sizeof(dict_size), 1);
            for (unsigned int i = 0; i < dict_size; i++) {
                unsigned short term_size;
                unsigned int list_size;
                in.fread(&term_size, sizeof(term_size), 1);
                in.fread(&list_size, sizeof(list_size), 1);
                std::string term(term_size, '\0');
                in.fread(term.data(), 1, term_size);
                for (unsigned int j = 0; j < list_size; j++) {
                    unsigned char buffer[BUFFER_LIMIT];
                    read_vbe(in.get(), buffer);
                    vbe_decode(buffer);
                    unsigned int freq;
                    in.fread(&freq, sizeof(freq), 1);
                    ++legacy_n;
                }
            }
        }
        const double legacy_s = seconds_since(start);

        start = clock_type::now();
        {
            BufferedReader in(path, STREAM_READ_BUFFER_SIZE);
            unsigned int dict_size;
            in.read(dict_size);
            for (unsigned int i = 0; i < dict_size; i++) {
                unsigned short term_size;
                unsigned int list_size;
                in.read(term_size);
                in.read(list_size);
                std::string term(term_size, '\0');
                in.fread(term.data(), sizeof(char), term_size);
                for (unsigned int j = 0; j < list_size; j++) {
                    unsigned long long delta;
                    unsigned int freq;
                    in.read_vbe(delta);
                    in.read(freq);
                    ++buffered_n;
                }
            }
        }
        const double buffered_s = seconds_since(start);

        if (legacy_n != buffered_n) std::cerr << "record count mismatch\n";
        report(mode, buffered_n, legacy_s, buffered_s);
    }
    else if (mode == "doclen") {
        unsigned long long legacy_n = 0;
        std::vector<unsigned int> legacy_list, buffered_list;

        auto start = clock_type::now();
        {
            SafeFile in(path, "rb");
            unsigned long long doc_id = 0, delta;
            unsigned int freq;
            while (read_doc_len_entry(in, &delta, &freq)) {
                doc_id += delta;
                if (doc_id >= legacy_list.size()) legacy_list.resize(doc_id + 1, 0);
                legacy_list[doc_id] = freq;
                ++legacy_n;
            }
        }
        const double legacy_s = seconds_since(start);

        start = clock_type::now();
        read_doc_len_list(path, buffered_list);
        const double buffered_s = seconds_since(start);

        if (legacy_list != buffered_list) std::cerr << "doc length mismatch\n";
        report(mode, legacy_n, legacy_s, buffered_s);
    }
    else if (mode == "blockmeta") {
        auto start = clock_type::now();
        auto terms = read_block_meta_file(path);
        const double seconds = seconds_since(start);
        unsigned long long blocks = 0;
        for (const auto& [term, meta] : terms) blocks += meta.block_meta_list.size();
        std::cout << std::format(
            "blockmeta terms={} blocks={} read_block_meta_file={:.2f}s ({:.0f} ns/block)\n",
            terms.size(), blocks, seconds, seconds / blocks * 1e9
        );
    }
    else {
        std::cerr << "Unknown mode " << mode << "\n";
        return 1;
    }
    return 0;
}
