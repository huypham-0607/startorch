#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>
#include <pybind11/chrono.h>
#include "startorch/retrieval/query_engine.h"
#include "startorch/retrieval/construct_doc_len_list.h"
#include "startorch/retrieval/construct_inverted_blocks.h"
#include "startorch/retrieval/merge_inverted_blocks.h"
#include "startorch/retrieval/file_names.h"

namespace py = pybind11;

PYBIND11_MODULE(startorch_cpp, m) {
    auto fn = m.def_submodule("file_names", "Centralized on-disk naming convention.");
    fn.attr("BLOCK_META") = file_names::BLOCK_META;
    fn.attr("METADATA_TXT") = file_names::METADATA_TXT;
    fn.attr("METADATA_BIN") = file_names::METADATA_BIN;
    fn.attr("DOC_LEN_LIST") = file_names::DOC_LEN_LIST;
    fn.attr("DOC_LEN_META") = file_names::DOC_LEN_META;
    fn.def("posting_file_name", &file_names::posting_file_name, py::arg("file_index"));
    fn.def("partial_block_file_name", &file_names::partial_block_file_name, py::arg("block_index"));

    m.def(
        "build_doc_len",
        &construct_doc_len_list,
        py::arg("in_dir"), py::arg("out_dir"),
        "Construct document length list to support BM25 score computation."
    );

    m.def(
        "build_inverted_blocks",
        &construct_inverted_blocks,
        py::arg("in_dir"), py::arg("out_dir"), py::arg("mem_limit"),
        "Construct SPIMI partial inverted index blocks."
    );

    m.def(
        "merge_inverted_blocks",
        &merge_inverted_blocks,
        py::arg("in_dir"), py::arg("out_dir"),
        py::arg("k1"), py::arg("b"), py::arg("block_size"), py::arg("split_size"),
        "Merge partial inverted index blocks into complete posting lists. "
        "out_dir must already contain doc_len_list.bin and doc_len_meta.bin."
    );

    py::class_<QueryEngine>(
        m, "QueryEngine",
        "An opened index. Constructing it does the whole index load once; "
        "keep one engine and call query()/query_exhaustive() repeatedly."
    )
        .def(
            py::init<const fs::path&>(),
            py::arg("meta_path"),
            "Load the index whose metadata.bin is at meta_path."
        )
        .def(
            "query",
            &QueryEngine::query,
            py::arg("terms"), py::arg("k"),
            "Run a Block-Max WAND top-k BM25 query. Returns (results, elapsed): "
            "results is a list of (score, doc_id) pairs best-first, elapsed is "
            "this query's search time (index load not included)."
        )
        .def(
            "query_exhaustive",
            &QueryEngine::query_exhaustive,
            py::arg("terms"), py::arg("k"),
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