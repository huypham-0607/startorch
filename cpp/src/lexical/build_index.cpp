#include "startorch/lexical/build_index.h"
#include "startorch/lexical/construct_doc_len_list.h"
#include "startorch/lexical/construct_inverted_blocks.h"
#include "startorch/lexical/file_names.h"
#include "startorch/lexical/merge_inverted_blocks.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/logger.h"

#include <format>

namespace fs = std::filesystem;

void build_index(
    const fs::path& token_dir,
    const fs::path& partial_dir,
    const fs::path& out_dir,
    const BuildParams& params
) {
    params.validate();
    Logger logger(__FILE_NAME__, Logger::INFO);

    fs::create_directories(partial_dir);
    fs::create_directories(out_dir);

    const std::vector<fs::path> stale = glob_files(partial_dir, "block_", file_names::PARTIAL_BLOCK_EXT);
    if (!stale.empty()) {
        logger.log(std::format("Removing {} partial blocks left in {} by an earlier build.", stale.size(), partial_dir.string()));
        for (const fs::path& path : stale) fs::remove(path);
    }

    logger.log("Started SPIMI pass, counting document lengths in the same pass.");
    DocLenCounter doc_lens;
    construct_inverted_blocks(token_dir, partial_dir, params.mem_limit, &doc_lens);
    doc_lens.write(out_dir);

    merge_partial_blocks(
        partial_dir,
        out_dir,
        doc_lens.lengths,
        doc_lens.total_docs(),
        doc_lens.total_frequency,
        params
    );
    logger.log("Finished building index.");
}
