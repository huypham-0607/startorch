#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>
#include <pybind11/chrono.h>
#include "startorch/lexical/query_engine.h"
#include "startorch/lexical/build_index.h"
#include "startorch/lexical/build_params.h"
#include "startorch/lexical/merge_inverted_blocks.h"
#include "startorch/lexical/file_names.h"

namespace py = pybind11;

// Python type checkers read python/src/startorch/_native/startorch_cpp.pyi, not
// this module. Keep that stub in sync with every change here.
PYBIND11_MODULE(startorch_cpp, m) {
    auto fn = m.def_submodule("file_names", "Centralized on-disk naming convention.");
    fn.attr("BLOCK_META") = file_names::BLOCK_META;
    fn.attr("METADATA_TXT") = file_names::METADATA_TXT;
    fn.attr("METADATA_BIN") = file_names::METADATA_BIN;
    fn.attr("DOC_LEN_LIST") = file_names::DOC_LEN_LIST;
    fn.attr("DOC_LEN_META") = file_names::DOC_LEN_META;
    fn.def("posting_file_name", &file_names::posting_file_name, py::arg("file_index"));
    fn.def("partial_block_file_name", &file_names::partial_block_file_name, py::arg("block_index"));

    // Keyword defaults come from BuildParams, the only place they live.
    const BuildParams defaults;
    m.def(
        "build_index",
        [](const fs::path& token_dir, const fs::path& partial_dir, const fs::path& out_dir,
           float k1, float b, int block_size, size_t split_size, size_t mem_limit) {
            BuildParams params;
            params.k1 = k1;
            params.b = b;
            params.block_size = block_size;
            params.split_size = split_size;
            params.mem_limit = mem_limit;
            build_index(token_dir, partial_dir, out_dir, params);
        },
        py::arg("token_dir"), py::arg("partial_dir"), py::arg("out_dir"),
        py::arg("k1") = defaults.k1, py::arg("b") = defaults.b,
        py::arg("block_size") = defaults.block_size, py::arg("split_size") = defaults.split_size,
        py::arg("mem_limit") = defaults.mem_limit,
        "Build a complete index from token_dir's token_*.bin files, reading them once: "
        "SPIMI partial blocks go to partial_dir (leftover ones are removed first), "
        "document lengths are counted in the same pass, and the merged index is "
        "written to out_dir. Parameters are validated first; an out-of-range value "
        "raises ValueError."
    );

    // The constructor and both queries release the GIL while they run. Safe:
    // pybind11 converts the arguments to C++ before releasing it and converts
    // the result back after re-acquiring it, so no Python object is touched in
    // between. query()/query_exhaustive() are const and thread-safe (see
    // QueryEngine), so Python threads may call them on one engine at once.
    py::class_<QueryEngine>(
        m, "QueryEngine",
        "An opened index. Constructing it does the whole index load once; "
        "keep one engine and call query()/query_exhaustive() repeatedly. "
        "Thread-safe; the GIL is released while loading and searching."
    )
        .def(
            py::init<const fs::path&>(),
            py::arg("meta_path"),
            py::call_guard<py::gil_scoped_release>(),
            "Load the index whose metadata.bin is at meta_path."
        )
        .def(
            "query",
            &QueryEngine::query,
            py::arg("terms"), py::arg("k"),
            py::call_guard<py::gil_scoped_release>(),
            "Run a Block-Max WAND top-k BM25 query. Returns (results, elapsed): "
            "results is a list of (score, doc_id) pairs best-first, elapsed is "
            "this query's search time (index load not included)."
        )
        .def(
            "query_exhaustive",
            &QueryEngine::query_exhaustive,
            py::arg("terms"), py::arg("k"),
            py::call_guard<py::gil_scoped_release>(),
            "Like query, but scores every candidate document with no pruning. "
            "Same return shape. Ground truth for query: results must match "
            "exactly, only elapsed should differ."
        );

    m.def(
        "read_term_df_mapping",
        &read_term_df_mapping,
        py::arg("meta_path"),
        "Return a vector of (term, df) mapping."
    );
}