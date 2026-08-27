# Startorch

**Startorch** is a literature search engine built around lexical retrieval and citation-graph ranking.

It currently targets the OpenAlex English-only Works corpus: 345M+ papers. The retrieval layer uses BM25 with a custom Block-Max WAND implementation, while the graph side will use Global PageRank to rerank retrieved documents.

The long-term goal is to support sub-second top-k retrieval over the full corpus while using the citation graph as an additional ranking signal, all while keeping RAM usage manageable for a personal device.

## Why this project?

Startorch is an experiment aimed to answer the question whether a combination of:

- Lexical relevance,
- Global graph authority, and
- Query-specific graph authority

can produce useful literature recommendations while remaining efficient enough to run over a graph with hundreds of millions of papers.

Admittedly, applications of PageRank in citation graph is not a novel concepts, and many papers in the past have aimed to answer this very question (e.g. CiteRank). The project is therefore more of an excuse to implement the underlying systems rather than treating retrieval and ranking as black boxes. In particular, the current work focuses on cleverly implementing IR pipelines to run under specific time and memory constraints.

## Status

As of August 18, 2026:

### Data pipeline : Done

The full OpenAlex Works corpus has been downloaded, extracted, compacted, and validated. The resulting dataset contains 510M+ works and occupies about 103.4 GB.

See [`docs/data_pipeline.md`](docs/data_pipeline.md).

### Retrieval engine: Done

The C++ retrieval engine is implemented and tested.

It currently supports:

- tokenization and dense document-ID remapping
- SPIMI-based inverted-index construction
- BM25 scoring
- Block-Max WAND query processing
- block-level upper-bound metadata
- variable-byte encoded posting lists

Index construction and querying are both working in C++, with 154 GoogleTest cases passing.

The engine is connected to Python CLI, which simple APIs for build posting lists, fetching data, and executing queries.

Retrival engine is benchmarked in rank-safety, retrieval effectiveness, and query latency using both OpenAlex English subset and MS MARCO Passage Ranking datasets.

See [`docs/bmw_technical_report.md`](docs/bmw_technical_report.md).

### Graph ranking

Not started yet.

Planned components are:

- CSR/CSC storage for the citation graph
- Global PageRank
- Some kind of query-specific authority (This needs more research into)

### Development dataset

To avoid iterating against the full 510M-node corpus during development, the repository can build a Mathematics-only OpenAlex subgraph containing roughly 4.7M works and their real citation edges.

This is intended for correctness testing and exploratory work in general.

### Python tooling

A first version of the `startorch` CLI and shared `project-config.toml` configuration file are in place.

The CLI currently supports the full retrieval pipeline, including data ingestion, test-subgraph generation, building inverted indicies & posting lists, and query engine.

### Semantic retrieval

Embedding-based retrieval is currently out of scope.

Original plan was to combine lexical, graph, and embedding-based retrieval, but running all three at OpenAlex scale would make the project considerably larger (And given the amount of manpower, not really feasible). For now, the focus is BM25/Block-Max WAND plus graph ranking.

## Next steps

The future plan for this project is:

1. Begin the graph representation and Global PageRank implementation.
2. Figure out to aggregate rankings from each metrics.
3. More research and reading for query-specific authority rankings.
4. Potentially revisit retrieval design to optimize for performance, particularly in memory usage.

## Progress

- [x] OpenAlex data pipeline
- [x] tokenization and normalization
- [x] dense document-ID remapping
- [x] Mathematics development subgraph
- [x] C++20/CMake build system
- [x] logging and shared C++ utilities
- [x] SPIMI inverted-index construction
- [x] Block-Max WAND metadata construction
- [x] Block-Max WAND query engine
- [x] BM25 scoring
- [x] C++ retrieval tests
- [x] connect Python CLI to C++ retrieval
- [x] benchmark retrieval engine
- [ ] CSR/CSC citation-graph representation
- [ ] Global PageRank
- [ ] approximate Personalized PageRank
- [ ] public benchmark evaluation

## Repository layout

Items marked ✅ are implemented and working. Items marked ⏳ exist but are incomplete.

```text
startorch/
├── cpp/
│   ├── CMakeLists.txt                  ✅ C++20 build, ctest integration
│   ├── include/
│   ├── src/
│   │   ├── utils/                      ✅ mmap/file wrappers, logging, VByte encoding
│   │   ├── retrieval/                  ✅ SPIMI index + Block-Max WAND query engine
│   │   └── graph/                      CSR/CSC, PageRank, PPR
│   ├── apps/                           ✅ index-construction command-line tools
│   ├── tests/                          ✅ 134 GoogleTest cases
│   └── benchmarks/                     benchmark datasets and future harnesses
│
├── python/
│   ├── src/startorch/
│   │   ├── cli.py                      ✅ main `startorch` CLI
│   │   ├── ingest/
│   │   │   └── fetch_data.py           ✅ OpenAlex ingestion
│   │   ├── works_subset/
│   │   │   └── works_subset.py         ✅ development-subgraph generation
│   │   ├── tokenizer/
│   │   │   └── tokenizer.py            ✅ tokenization + document-ID remapping
│   │   └── utils.py                    ✅ shared helpers
│   ├── script/
│   │   └── smoke_test.py               ✅ CLI smoke tests
│   └── notebook/                       exploratory analysis
│
├── docs/
│   ├── initialization.md               overall project plan
│   ├── algorithm_design.md             retrieval/ranking design (deprecated)
│   ├── retrieval_engine.md             BM25 + Block-Max WAND implementation
│   ├── bmw_technical_report.md         Comprehensive BMW implementation technical report + benchmarks.
│   ├── data_pipeline.md                OpenAlex ingestion pipeline
│   └── data_reference.md               OpenAlex field reference
│
├── project-config.toml                 ✅ shared paths and subset configuration
├── data/                               local data; gitignored
└── README.md
```

A more detailed file-by-file description is available in [`docs/algorithm_design.md`](docs/algorithm_design.md).

## References

- Brin, S. and Page, L.  
  [*The Anatomy of a Large-Scale Hypertextual Web Search Engine*](https://snap.stanford.edu/class/cs224w-readings/Brin98Anatomy.pdf)

- Austin, D.  
  [*The $25,000,000,000 Eigenvector: The Linear Algebra Behind Google*](https://www.rose-hulman.edu/~bryan/googleFinalVersionFixed.pdf)

- Langville, A. and Meyer, C.  
  [*Deeper Inside PageRank*](https://www.stat.uchicago.edu/~lekheng/meetings/mathofranking/ref/langville.pdf)

- Robertson, S. and Zaragoza, H.  
  [*The Probabilistic Relevance Framework: BM25 and Beyond*](https://www.staff.city.ac.uk/~sbrp622/papers/foundations_bm25_review.pdf)

- **WAND (Weak AND)** — Broder, Carmel, Herscovici, Soffer, Zien, [*"Efficient Query Evaluation using a Two-Level Retrieval Process"*](https://www.researchgate.net/publication/221613425_Efficient_query_evaluation_using_a_two-level_retrieval_process)
  (CIKM 2003).

- **Block-Max WAND (BMW)** — Ding & Suel, [*"Faster Top-k Document Retrieval Using Block-Max Indexes"*](https://research.engineering.nyu.edu/~suel/papers/bmw.pdf)
  (SIGIR 2011). 

- [PISA](https://github.com/pisa-engine/pisa) 

- Manning, Raghavan & Schütze, *Introduction to Information Retrieval*
