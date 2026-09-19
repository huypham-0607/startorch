# Algorithm Design for Startorch

<!-- Funny how this became a text-retrieval project from an authority ranking project -->

## 1. Abstract

Key principals for a Search Engine
- Relevance - Search result must be highly related to the given query. Achievable through keyword matching/semantic relevance.
- Authority - Search result must be highly credible/high quality sources. Achievable through various graph-based ranking algorithm (PR, HITS, SALSA).

## 2) Preliminary Design

A document score relative to a given query is a combination of multiple (normalized) scoring factors.

### 2.1) Base scoring formmulation

$S(d,q) = R(d,q)(w_{R} + w_{GA}GA(d)) + w_{LA}LA(d,q)$

- $d$ - Document being assessed/scored
- $q$ - User query
- $w_{x}$ - Specified weight for criteria $x$

Criteriation terms will be scaled and normalized accordingly. (Needed more research on how to effectively combine these metrics)

### 2.2) Relevance (R(d,q))

Compute relevance of a specific document relative to given query.

<!--
    Resolution before stage 2:
    - Explicitly mention how these 4 candidates combine for final criteria score.
-->

Potential candidates to quantify relevance:
- BM25 score (Title, topics & keywords - Abstract with low weight)
- embedding similarity/semantic relevance **(dropped for now — see §5.3)**
- Exact matches (Would weight higher for Title/keywords than abstract)
- Query-term coverage percentage

About abstract:
- Due to the very high percentages of NULL abstracts, abstract would only contributes a small portion to total weight for applicable metrics.
- Documents with NULL abstract would have their score normalized to compensate for missing abstract weight. (This, of course, must be further experiment and research depending on nature of specified metrics).

### 2.3) Local authority (LA(d,q))

Compute authority for a query-subsetted document graph.

Potential candidates to quantify local authority.
- Approximate top-k PPR / local push using seeds from High-recall candidate retrieval 
- Per-topic PPR score + topic semantic relevance to query. 
- Potentially some other metrics related to candidates after HRCR (?)

### 2.4) Global authority (GA(d))

Compute authority for a the global subgraph.

Obviously, Global PageRank will be the main engine behind this metric.

Other potential candidates
- HITS/SALSA (limited research currently)

Notes:
- As per the formula referenced in section 2.1, $GA(d)$ is scaled with $R(d,q)$. This is to ensure that generally credible but unrelated papers will not flood top results.
- Due to the near-DAG feature of citation graphs, we need a way to limit the effect of paper age for our PageRank. We can read [this paper about CiteRank](https://arxiv.org/abs/physics/0612122) for information and potential fix.

**Graph storage (CSR/CSC)**: PageRank's power iteration is repeated sparse matrix-vector multiplication, so
storage format directly determines which access pattern is fast. PageRank's natural update — $rank(v) = \sum$
over in-neighbors $u$ of $rank(u)/outdegree(u)$ — is a *gather from in-neighbors* operation, which wants
column-wise access (CSC), or equivalently CSR built over the graph's transpose. A scatter/push-style
implementation (distribute rank along out-edges instead) wants CSR of the graph as-is. Decide which
formulation is being implemented before deciding whether one representation suffices or both are needed —
not yet decided.

Resources (CSR/CSC structure often under-explained as "just a list of nonzero index-value pairs," which is
actually COO format — the real CSR/CSC insight is the `row_ptr`/`col_ptr` array giving O(1) indexed row/column
access, not linear scan):
- [Scientific Python Lectures: CSR](https://lectures.scientific-python.org/advanced/scipy_sparse/csr_array.html) /
  [CSC](https://lectures.scientific-python.org/advanced/scipy_sparse/csc_array.html) — clearest explanation of
  the actual 3-array structure.
- [scipy `csr_array`/`csc_array` docs](https://docs.scipy.org/doc/scipy/reference/generated/scipy.sparse.csr_array.html) —
  good for hands-on verification in Python before implementing from scratch in C++.
- [GraphBLAS](https://graphblas.org/) / [SuiteSparse:GraphBLAS paper](https://people.engr.tamu.edu/davis/GraphBLAS_files/toms_graphblas.pdf) —
  the "graph algorithm = sparse linear algebra over a semiring" framing; PageRank is a canonical example.
- [nicholasRenninger/PageRank_Algorithm-CSR](https://github.com/nicholasRenninger/PageRank_Algorithm-CSR) — a
  hand-built C++ PageRank implementation over a hand-built CSR structure, not library-wrapped — closer to what
  this project is actually building than a scipy-based example.

**Note**: the same "sparse raw IDs are unusable as array indices" problem already got solved on the BMW side —
`tokenizer.py` ranks every document into a dense `[0,N)` `mapped_id` up front (`doc_id_lookup.bin`), and all
retrieval-side structures (posting lists, `doc_len_list.bin`, BMW block metadata) index by it. Whether the
graph's node IDs reuse that same mapped_id space or need their own numbering is still open — worth deciding by
reusing rather than re-solving, since it's the identical underlying question.

### 2.5) Additional/Experimental criterias (subject to change)
- F(d,q): Freshness score for a document/query pair, Higher score for more recent paper, and higher/lower score if query explicitly mention a timestamp.
- Q(d): Intrinsic Quality for a research paper. Hard to quantify this.

## 3. Pipeline

### Stage 1: High-recall candidate retrieval

Retrieve 500-2000 potential candidates using
- Lexical retrieval — a custom Block-Max WAND engine, not DuckDB's FTS extension (tried first, abandoned for
  being too slow at full corpus scale). Index construction and query-time traversal are both built and tested
  (134 C++ tests). Not yet callable from outside C++ — hooking it up to Python is the current next step. See
  `docs/retrieval_engine.md`.
    - BM25 over title/topic/keywords
    - Title/topic/keyword matches > Abstract matches
    - Compute exact entity matches
- Semantic retrieval — **dropped for now, see §5.3.** Design sketch below kept for reference if revisited later.
    - Embed query and document metadata
    - Retrieve based on similarity (Further research needed)
- Near neighbor extension (?)
    - Also retrieve close neighbor of potential candidates
    - **More load-bearing now that semantic retrieval is deferred** — this is the only mechanism left in Stage 1
      that can surface relevant papers not sharing vocabulary with the query. Worth promoting from "(?)" to a
      decided part of the design once PPR exists to drive it (seed from BM25 hits, walk the full graph via
      approximate top-k PPR — not PageRank restricted to the induced subgraph of just the candidates).

Scoring system - Subject to more research

### Stage 2: Rerank subsetted candidates

Uses scoring system mentioned in section 2.

## 4. Validation

### Building dataset

Usually for text retrieval, we validate results using a set of predetermined queries + expected document chunk (ie. Golden dataset)

Two potential sources:
- Manual dataset: Handpicked query/result pairs, precompiled data, etc.
- Synthetic generation: Leverage multiple LLM models to generate query/result pairs & cross-validate returned dataset (or potentially treating them as "votes").

### Quantify accuracy

Generally, a good score can quantify these attributes:
- No of matches in actual document chunk and expected document chunk.
- Relevance of top result in actual document chunk.
- How far down are top results from expected document chunk in actual document chunk.

Potential metrics:

- Recall@K
- nDCG@10
- MRR

Baseline Models:

- Pure BM25
- VSM

## 5) MVP scope cut:

### 5.1) Query adaptivity

- MVP version only support one fixed weight formula for all queries (ie. foundational vs advanced vs influential are all treated the same). This is intentional to reduce project complexity.
- Non-free-text query are not supported for now.

### 5.2) Explainable output

- ie. A set of strings explaining why a particular document is ranked high.
- Not the current main scope for now, but worth keeping in mind.

### 5.3) Semantic retrieval — fully dropped for now (2026-08-02)
- Would be too ambitous computationally.
- Perhaps would revisit in the future (after ~3 months at least)

#### Feasibility check (2026-09-18) — still dropped

Re-checked on request. The decision stands. The numbers below are here so the revisit doesn't have to redo them.

**Hardware.**
- Intel Core Ultra 7 255HX (20 cores), 31 GB RAM.
- RTX 5060 Laptop GPU with 8 GB of video memory.
- 379 GB free on `/data`, 433 GB free on `/home`.
- No ML libraries installed yet (torch, sentence-transformers, faiss).

**Full-en at scale** (345,897,793 documents). Estimates, not measured; the first step of any revisit is timing encoding on about 100k documents.

| | 384 dimensions | 768 dimensions |
|---|---|---|
| int8 vectors | 133 GB | 266 GB |
| fp16 vectors | 266 GB | 531 GB |
| 1-bit codes | 17 GB | 33 GB |
| GPU encoding, title + keywords | roughly 6-20 h (small model) | roughly 1-3 days (base model) |

- **An in-memory HNSW index does not fit.** Its layer-0 links alone take about 44 GB at M=16, before any vectors.
- **Indexes that do fit in 31 GB** keep compressed codes in RAM and full vectors on SSD:
  - IVF-PQ at 32-64 bytes per vector, about 11-22 GB;
  - 1-bit codes scanned brute force, then rescored from int8 vectors on SSD;
  - a DiskANN-style graph on SSD.

  All three compete for RAM with the query engine's own index load, about 8 GB on full-en.
- **Data caveat.** Documents have only a title, topic labels, and keywords; abstracts are excluded (47.72% null). The topic labels are shared across huge numbers of documents and would dominate the embeddings, so embed title plus keywords.

**Three tiers, cheapest first.**

1. **Cross-encoder rerank of BMW's top 100 at query time.**
   - No corpus encoding and no new storage.
   - Roughly tens of milliseconds per query on the GPU.
   - Only reorders documents BMW already found.
2. **Rerank using precomputed document embeddings.**
   - One full-corpus encoding and about 133 GB of int8 vectors.
   - Per query, only BMW's candidates are looked up by `mapped_id` and scored, which is almost free.
   - Same limit as tier 1.
3. **Dense first-stage retrieval, fused with BMW** (for example, reciprocal rank fusion).
   - The only tier that finds relevant papers sharing no vocabulary with the query.
   - Needs one of the compressed indexes above at full-en scale.

**When revisited, start with tier 3 plus fusion on MS MARCO.**
- Its 8,841,823 passages come to about 7 GB of 384-dimension fp16 vectors, so an in-memory HNSW index fits.
- The existing MS MARCO pipeline already produces run files, and MRR@10 and Recall@1000 compare directly with BMW and Anserini.
- Scale to full-en only if it clearly beats BMW.
- An ANN index (HNSW or IVF-PQ) in C++ is the natural hands-on part, like BMW was. The embedding model would come from a library.


# 6. Project tree

Items marked ✅ exist and work (tested); ⏳ exist but incomplete/stub only; unmarked = not started yet.

```
cpp/
├── CMakeLists.txt                      ✅ C++20, compile_commands.json, Release build, ctest wired up
├── include/startorch/
│   ├── utils/
│   │   ├── logger.h                    ✅
│   │   ├── vbe.h                       ✅ variable-byte encoding
│   │   └── file_io.h                   ✅ SafeFile + SafeFileMmap (RAII file/mmap wrappers), glob_files
│   ├── retrieval/
│   │   ├── posting_list.h              ✅ PostingItem, PostingList
│   │   ├── bm25.h                      ✅ calc_BM25, bm25_saturation
│   │   ├── token_stream.h              ✅ read_token (tokenizer wire format)
│   │   ├── construct_inverted_blocks.h ✅ SPIMI partial-block construction
│   │   ├── construct_doc_len_list.h    ✅ document-length table construction
│   │   ├── merge_inverted_blocks.h     ✅ BlockMeta/TermMeta, k-way merge, BMW block metadata + metadata file
│   │   └── query_engine.h              ✅ PostingPointer, WAND/BMW query loop
│   └── graph/                          # CSR/CSC structures, PageRank, PPR headers — not started
├── src/
│   ├── utils/                          ✅ mirrors headers above
│   ├── retrieval/                      ✅ mirrors headers above, including query_engine.cpp
│   └── graph/                          # not started
├── apps/
│   ├── build_inverted_blocks.cpp       ✅ CLI: tokenized input -> partial SPIMI blocks
│   ├── build_doc_len_list.cpp          ✅ CLI: tokenized input -> doc_len_list.bin
│   ├── merge_inverted_blocks.cpp       ✅ CLI: partial blocks -> posting files + metadata
│   └── (no CLI entry point for running a query yet)
├── tests/
│   ├── utils/                          ✅ vbe_tests, file_io_tests
│   ├── retrieval/                      ✅ one GoogleTest binary per source file above — 134 tests total
│   └── graph/
└── benchmarks/                         # ties to the BEIR/SNAP/OGB datasets already compiled — not started
```

The Python side is being refactored alongside this (see `README.md` for the current roadmap):

```
python/src/startorch/
├── cli.py                              ⏳ single startorch command; ingest + gen-works-subset done,
│                                            build-posting + query commands not added yet
├── utils.py                            ✅ shared logging/path helpers
├── ingest/fetch_data.py                ✅ EntityIngestor (base class) + WorksIngestor (the Works entity)
├── works_subset/works_subset.py        ✅ WorksSubsetter — filters the full corpus into a smaller test set
└── tokenizer/tokenizer.py              ✅ tokenization + doc_id remapping (works, but not CLI-driven yet)

project-config.toml                     ✅ paths + subset filter profiles, at the repo root
```