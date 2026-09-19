/**
 * @file merge_inverted_blocks.cpp
 * @brief CLI entry point: SPIMI partial-block merge (merge_inverted_blocks). Construct full
 * posting list & block metadata for corpus.
 */

#include "startorch/retrieval/build_params.h"
#include "startorch/retrieval/merge_inverted_blocks.h"
#include "startorch/utils/logger.h"

#include <filesystem>
#include <format>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    // Defaults and valid ranges both come from BuildParams, so this app and
    // the Python build accept exactly the same values.
    BuildParams params;

    std::string usage_err = std::format(
        "Usage: {} <in_dir> <out_dir> <optional_flags>\n"
        "out_dir must already contain doc_len_list.bin and doc_len_meta.bin\n"
        "(construct_doc_len_list output).\n"
        "Optional flags:\n"
        "k1=(float)                 - k1 parameter in BM25, (0, 5]. Default {}.\n"
        "b=(float)                  - b parameter in BM25, [0, 1]. Default {}.\n"
        "block_size=(int)           - Block partition size for Block-Max WAND, > 0. Default {}.\n"
        "split_size=(size_t)        - Splitting threshold for posting list serialization, > 0. Default {}.",
        argv[0], params.k1, params.b, params.block_size, params.split_size
    );

    if (argc < 3 || argc > 7) {
        std::cerr << usage_err << "\n";
        return 1;
    }

    fs::path in_dir = argv[1];
    fs::path out_dir = argv[2];

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        auto eq = arg.find('=');
        if (eq == std::string::npos) {
            std::cerr << usage_err << "\n";
            return 1;
        }
        std::string key = arg.substr(0, eq), val = arg.substr(eq + 1);
        try {
            if (key == "k1") params.k1 = std::stof(val);
            else if (key == "b") params.b = std::stof(val);
            else if (key == "block_size") params.block_size = std::stoi(val);
            else if (key == "split_size") params.split_size = std::stoull(val);
            else {
                std::cerr << std::format("Unknown flag {}.", key) << "\n";
                return 1;
            }
        }
        catch (const std::exception& e) {
            std::cerr << std::format("Cannot convert value {} for flag {}.", val, key) << "\n";
            return 1;
        }
    }

    try {
        params.validate();
    }
    catch (const std::invalid_argument& e) {
        std::cerr << e.what() << "\n" << usage_err << "\n";
        return 1;
    }

    Logger logger(__FILE_NAME__, Logger::INFO);

    try {
        fs::create_directories(out_dir);

        logger.log(std::format(
            "Merging inverted blocks from {} into {}.\n"
            "Params:\n"
            "k1 = {}\n"
            "b = {}\n"
            "block_size = {}\n"
            "split_size = {}",
            in_dir.string(),
            out_dir.string(),
            params.k1,
            params.b,
            params.block_size,
            params.split_size
        ));

        merge_inverted_blocks(in_dir, out_dir, params);

        logger.log("Finished merging inverted blocks.");
    } catch (const std::exception& e) {
        logger.log(std::format("Failed: {}", e.what()), Logger::ERROR);
        return 1;
    }

    return 0;
}
