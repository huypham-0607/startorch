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

As of September 21, 2026:

### Data pipeline : Done

The full OpenAlex Works corpus has been downloaded, extracted, compacted, and validated. The resulting dataset contains 510,372,821 works and occupies about 107 GB, extracted from a 780 GB raw snapshot.

The corpus is the OpenAlex June 2026 public release, fetched on August 13, 2026. The public snapshot is released quarterly, so the local copy drifts from the OpenAlex website between releases.

About 42% of the English corpus are OpenAlex expansion records, mostly datasets and repository items, which the OpenAlex website hides by default. Filtering them out of the index is still open.

See [`docs/data_pipeline.md`](docs/data_pipeline.md).

### Lexical retrieval engine: Done

The C++ lexical retrieval engine is implemented and tested.

It currently supports:

- tokenization and dense document-ID remapping
- SPIMI-based inverted-index construction
- BM25 scoring
- Block-Max WAND query processing
- block-level upper-bound metadata
- variable-byte encoded posting lists

Index construction and querying are both working in C++, with 198 GoogleTest cases passing. The whole index is built in a single pass over the token stream.

The engine is connected to Python CLI, which simple APIs for build posting lists, fetching data, and executing queries.

Lexical retrival engine is benchmarked in rank-safety, retrieval effectiveness, and query latency using both OpenAlex English subset and MS MARCO Passage Ranking datasets. On MS MARCO Passage Ranking it reaches MRR@10 0.1925 and Recall@1000 0.8778, slightly above Anserini BM25 at 0.1892 and 0.8573.

On the full English corpus, the loaded engine holds 6.7 GiB in memory, and needs about 8.7 GiB while loading. Posting lists are memory-mapped on top of that. A 16 GB machine is therefore the practical minimum.

See [`docs/bmw_technical_report.md`](docs/bmw_technical_report.md).

### Live service: In progress

The HTTP API works and is tested locally. Deployment to AWS is not started.

It currently supports:

- `/search`, `/healthz`, and `/readyz` endpoints
- background index loading, so health checks answer during the load
- up to 4 searches in parallel, on worker threads
- limits on `k` and query length

The query engine and tokenizer are thread-safe, and the engine releases the GIL while loading and searching. The C++ side is checked with ThreadSanitizer.

Results carry OpenAlex IDs only; the client fetches titles from the OpenAlex API. Next steps are a Docker image, then the index in S3, served from a single EC2 instance.

See [`docs/deployment.md`](docs/deployment.md).

### Graph ranking

Not started yet.

Planned components are:

- CSR/CSC storage for the citation graph
- Global PageRank
- Some kind of query-specific authority (This needs more research into)

### Development dataset

To avoid iterating against the full 510M-node corpus during development, the repository can build a Mathematics-only OpenAlex subgraph containing roughly 4.7M works and their real citation edges.

This is intended for correctness testing and exploratory work in general. Engineering, veterinary, and computer-science subsets are defined the same way in `project-config.toml`.

### Python tooling

A first version of the `startorch` CLI and shared `project-config.toml` configuration file are in place.

The CLI currently supports the full retrieval pipeline, including data ingestion, test-subgraph generation, building inverted indicies & posting lists, and query engine.

The package is organized by capability: `ingest`, `lexical`, `utils`, and `api` for the HTTP layer. Graph and semantic code will sit beside them.

A pytest suite under `python/tests` covers the package wiring, the CLI, the tokenizer, search, and the HTTP API, including thread safety. Tests that need the MS MARCO index skip themselves when it is not built.

### Semantic retrieval

Embedding-based retrieval is currently out of scope.

Original plan was to combine lexical, graph, and embedding-based retrieval, but running all three at OpenAlex scale would make the project considerably larger (And given the amount of manpower, not really feasible). For now, the focus is BM25/Block-Max WAND plus graph ranking.

## Next steps

The future plan for this project is:

1. Deploy the query engine as a live service on EC2 and S3.
2. Begin the graph representation and Global PageRank implementation.
3. Figure out to aggregate rankings from each metrics.
4. More research and reading for query-specific authority rankings.
5. Reduce query-engine memory, so the full corpus fits on a smaller machine.

## Progress

- [x] OpenAlex data pipeline
- [x] tokenization and normalization
- [x] dense document-ID remapping
- [x] Mathematics development subgraph
- [x] C++20/CMake build system
- [x] logging and shared C++ utilities
- [x] SPIMI inverted-index construction
- [x] single-pass index construction
- [x] Block-Max WAND metadata construction
- [x] Block-Max WAND query engine
- [x] BM25 scoring
- [x] C++ retrieval tests
- [x] connect Python CLI to C++ retrieval
- [x] benchmark retrieval engine
- [x] HTTP API layer
- [ ] EC2 + S3 deployment
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
│   │   ├── lexical/                    ✅ SPIMI index + Block-Max WAND query engine
│   │   └── graph/                      CSR/CSC, PageRank, PPR
│   ├── python/bindings.cpp             ✅ pybind11 module (startorch_cpp)
│   ├── tests/                          ✅ 198 GoogleTest cases
│   └── benchmarks/                     benchmark datasets and future harnesses
│
├── python/
│   ├── src/startorch/
│   │   ├── cli.py                      ✅ main `startorch` CLI
│   │   ├── ingest/                     ✅ fetch_data.py, works_subset.py
│   │   ├── lexical/                    ✅ tokenizer.py, build_posting.py,
│   │   │                                  doc_id_lookup.py, search.py
│   │   ├── utils/                      ✅ paths.py, logger.py, misc.py, duckdb.py
│   │   ├── api/                        ✅ FastAPI app + response models
│   │   └── _native/                    ✅ compiled startorch_cpp + type stub
│   ├── benchmark/                      ✅ query sets, latency/correctness runs, report/
│   ├── tests/                          ✅ pytest suite: package, CLI, tokenizer, search, API
│   └── notebook/                       exploratory analysis
│
├── docs/
│   ├── initialization.md               overall project plan
│   ├── algorithm_design.md             retrieval/ranking design (deprecated)
│   ├── retrieval_engine.md             BM25 + Block-Max WAND implementation
│   ├── bmw_technical_report.md         Comprehensive BMW implementation technical report + benchmarks.
│   ├── data_pipeline.md                OpenAlex ingestion pipeline
│   ├── deployment.md                   EC2 + S3 deployment plan
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
