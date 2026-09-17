# Startorch refactor plan

Last updated 2026-09-17. Tracks the refactor that came out of the full-repo design review: how the pipeline works today, what has been fixed, and what is left.

The recurring problem is that library functions bake in one corpus, one path, or one output mode. Each new use then copies the function instead of calling it. Copies drift, and several already have.

## 1. Current project flow

Startorch is currently a lexical search pipeline over English OpenAlex works: raw snapshot in, top-k BM25 results out. The graph ranking stage that would rerank those results is not built yet.

The four CLI commands run in this order:

```
startorch ingest --entity works
startorch gen-works-subset --profile full-en
startorch build-posting --profile full-en
startorch query --query "..." --k 10 --profile full-en
```

| Step | What it does | Output |
|---|---|---|
| Ingest | Downloads the OpenAlex Works snapshot one shard at a time. Each shard is cut down to the needed fields, validated, and saved as compact Parquet, then the raw shard is deleted. | `full_corpus/works/*.parquet` |
| Subset | Filters the full corpus with the profile's SQL condition from `project-config.toml`, then validates the result. `full-en` keeps all English works. | `works_subset/<profile>/*.parquet` |
| Build | Python gives every document a dense integer id, builds its text from title, topic hierarchy, and keywords, strips stopwords, stems, and writes a binary token stream. C++ counts document lengths, builds SPIMI partial indexes under a memory cap, and merges them. The C++ steps accept documents in any doc id order. The merge computes avgdl and each block's maximum possible BM25 score. | `doc_id_lookup.bin`, `token_*.bin`, `doc_len_list.bin`, `posting_*.bin`, `block_meta.bin`, `metadata.bin` / `.txt` |
| Query | Tokenizes the query with the same rules. `QueryEngine` loads metadata (including avgdl), document lengths, and block metadata into memory, memory-maps the posting files, and runs Block-Max WAND. Python maps the returned ids back to OpenAlex ids and prints them. | stdout |

Each CLI `query` call is a new process, so it still pays the full index load, about 96 s on full-en. The benchmarks keep one engine for a whole query set.

Benchmarking and reporting live outside the CLI:

- `query_gen.py` generates synthetic query sets from term document frequencies.
- `bmw_performance.py` and `bmw_correctness.py` run those sets through the pruned and exhaustive engines, appending latency, memory, and result comparisons to Parquet files.
- `ms_marco_pipeline.py` builds a separate MS MARCO index so result quality can be scored against Anserini.
- `report/prepare.py` combines the Parquet files into `agg.json`, and `report/render.py` renders tables. Those numbers are currently copied by hand into `bmw_technical_report.md`.

Config is split between `project-config.toml` for the main pipeline and `python/benchmark/benchmark-config.toml` for the scripts. All data lives under `/data`, outside the repo.

## 2. Done

### 2026-09-17: refactor round 1

Five items from `refractor_scratch.md`. None of them changes the on-disk index format, so the full-en index being rebuilt now (PID 40162) stays loadable. Some verification waits for that rebuild to finish; see §3.

#### One index loader

**Problem.** The query engine constructor and `read_term_df_mapping` each repeated the same steps: read `metadata.bin`, then read `block_meta.bin` from the posting folder. The old bug that read block metadata from a directory path lived in exactly this sequence. Both also trusted the absolute paths stored in `metadata.bin`, which go stale once an index folder is renamed.

**Fix.**

- `load_index(meta_path)` in `merge_inverted_blocks.cpp` returns an `IndexMeta` holding the posting folder, k1, b, avgdl, block size, split size, and every term's metadata. Both callers use it. The engine then also loads document lengths; the term-df reader doesn't need them.
- It resolves every file relative to the folder holding `metadata.bin`. The merge always writes metadata into the posting folder itself, so existing indexes resolve to the same files as before. The stored paths are still read, so the format is unchanged, but they are ignored.

#### Duplicated logic inside the engine

- `PostingPointer::find_block` holds the block search that `next_shallow` and `next_shallow_deep` both used.
- `advance_one_excluding` and `advance_one_including` are now one template, `advance_one(..., cmp)`. The two call sites pass `std::less<>` and `std::less_equal<>`, each with a one-line comment on why. The old "including" error message wrongly said `<`.
- Pruned and exhaustive search share `open_postings`, `make_top_k`, and `drain_top_k`, and keep only their own loops.

#### Query engine as one Python class

**Problem.** Four near-identical free functions (`query`, `query_batch`, `query_batch_benchmark`, `query_batch_exhaustive_benchmark`) each built a new engine, and the exhaustive benchmark function differed from its sibling by one method call.

**Fix.**

- `QueryEngine` is declared in `query_engine.h` and bound to Python as a class. `QueryEngine(meta_path)` loads the index once. `query(terms, k)` and `query_exhaustive(terms, k)` each return `(results, elapsed)`.
- The C++ timer covers only the search, as before, so new latencies are comparable with recorded ones.
- The four free functions and their bindings are deleted. `query_wrapper.retrieve`, `full_en_bench_query.py`, and `ms_marco_pipeline.py` hold one engine and loop over queries, timing the constructor in Python for index-load latency. The `run_queries*` helpers return the same shapes, so `bmw_performance.py` and `bmw_correctness.py` needed no code changes.
- Not yet run from Python: the `startorch_cpp` module can't be rebuilt while the running rebuild has the old `.so` mapped. See §3.

#### Tokenizer memory fix, Option 3 (C++ half done, Python half pending)

**Problem.** To hand C++ a stream in doc id order, the tokenizer joins every document to its dense id, sorts every document's token list, and materializes every token as a row in a DuckDB temp table: about 13.7B rows on full-en. Under a 512 MB DuckDB memory cap, this ran out of memory on the full math-en subset. A sorted Arrow stream with no temp table completed under the same cap, but still spilled about 1.3 GB for its sort. The unsorted stream planned here has not been measured yet.

**Decision.** Keep corpus file order and make the C++ build accept any doc order.

**C++ (done).**

- `write_partial_index` sorts each term's posting list by doc id before writing gaps, merging repeated doc ids (`PostingList::sort`). This happens in memory, within the SPIMI cap, and is skipped for lists that are already increasing.
- `construct_doc_len_list` counts tokens into an array indexed by doc id, then writes the non-zero entries in doc id order. That is about 1.4 GB on full-en, the same array the merge and the query engine already hold. The earlier estimate that the doc-length file format would change was wrong; it is unchanged.
- `build_posting_list` is unchanged. Its heap merge already accepts partial blocks whose doc id ranges overlap, and already merges a document split across two partial blocks. Sorting there would have held a whole posting list in memory, about 5.5 GB for the most common full-en terms.
- A stream already in doc id order builds exactly as before. That covers the MS MARCO tokenizer fork and the current full-en rebuild.

**Python (pending, gated on the spill check in §3).** Changes to `tokenizer.py`:

- Build the lookup file from an id-only ranking, run once instead of twice, written per batch with numpy.
- Stream `(raw id, tokens)` rows in file order with no ORDER BY, JOIN, or temp table.
- Map raw ids to dense ids per batch with `np.searchsorted` on the sorted raw ids (about 2.8 GB). Validation shows full-en has no duplicate ids, so every raw id maps to exactly one dense id.
- Encode each batch with numpy instead of calling `struct.pack` per token.

#### Profile spelling (code done, folder renames pending)

- A profile's name is its folder name everywhere. The translation function `get_subset_folder` is deleted, and the `full_en` path literals in `works_subset.py`, `full_en_bench_query.py`, `query_gen.py`, and `benchmarks.ipynb` now say `full-en`.
- Python module and identifier names such as `full_en_bench_query.py` keep underscores, because modules can't be imported with dashes. Data file names such as `full_en_index_stats.json` are not profile paths and keep theirs too.
- The six `/data` folders still use underscores until they are renamed (§3). Until then the CLI and benchmarks can't find them.

#### Tests

- C++: 169 of 169 pass, up from 159.
  - Loader: returns the stored parameters and terms; still loads after the index folder is renamed (tested for both `load_index` and `QueryEngine`); `read_term_df_mapping` is sorted by df.
  - Engine class: a reused engine matches fresh engines; each query honours its own k; unmatched and empty queries return nothing; `query_exhaustive` matches `query` across several queries and k values with one-posting blocks.
  - Doc order: `PostingList::sort`; unsorted streams through SPIMI and doc lengths; and an end-to-end build where sorted and shuffled doc order, split across several partial blocks and posting files, produce byte-identical `doc_len_list.bin`, `doc_len_meta.bin`, `block_meta.bin`, and `posting_*.bin`.
  - Replaced: the four `query_batch*` tests.
- Smoke test: 14 of 14 checks pass. It covers imports and CLI `--help` only, not the new binding.

### 2026-09-16: avgdl, IDF, smoke test constant

#### Unified avgdl

**Problem.** avgdl was computed twice, with different denominators. 196,626 full-en documents produced no tokens and leave gaps in the doc-length array.

| Step | Divided total tokens by | avgdl (full-en) |
|---|---|---|
| Index build, for block upper bounds | doc-length array length (≈345,897,793) | 39.7048 |
| Query time, for real scores | documents with tokens (345,701,167) | 39.7273 |

The smaller build-time avgdl made stored block bounds slightly lower than real query-time scores. So the bounds were not true upper bounds, and BMW could prune a document it should keep at near-ties. This is a likely contributor to the missing documents in report §5.1, not a proven cause.

**Fix.**

- The merge step computes avgdl once, as total tokens over documents with tokens. That is the value query time already used, so real scores keep the same avgdl and only block bounds move, in the safe direction.
- avgdl is written to `metadata.bin` and `metadata.txt`, and `read_metadata` returns it.
- The query engine reads avgdl from metadata instead of recomputing it, and no longer reads `doc_len_meta.bin`.
- N was already `doc_len_list.size()` in both paths, so it is unchanged and not stored.

#### IDF formula

**Problem.** The code used `ln(N / df)`. The docs, and Anserini, use `ln((N - df + 0.5) / (df + 0.5) + 1)`. IDF was also implemented separately in `bm25.cpp` and in the merge step.

**Fix.** `bm25_idf(N, df)` in the BM25 module is now the only IDF implementation, and both the merge step and `calc_BM25` call it. It uses the documented formula, computed in double precision through `log1p`, because N exceeds the range a float stores exactly. It stays positive when a term appears in every document.

#### Smoke test constant

**Problem.** `smoke_test.py` asserted a default chunk size of 10,000,000, but the real default is 2,000,000, so the test failed.

**Fix.** The expected value is now 2,000,000. Nothing else in the file changed.

#### Tests

- C++: 159 of 159 pass, up from 154.
  - New: four IDF tests (hand-computed value, positive when df equals N, monotonic in df, accurate at corpus scale).
  - New: a regression test on an index with doc-id gaps. It checks the stored avgdl and that no real score exceeds its block bound.
  - Updated: the merge test's hardcoded bound values, recomputed in float32 for the new IDF, and the metadata round-trip tests, which now include avgdl.
- Smoke test: 14 of 14 checks pass.

#### Consequences

- **Every existing index under `/data` must be rebuilt.** The metadata format changed, and old files now fail to load with "Failed to read file" (verified on math-en). The stored block bounds also depend on the IDF formula. The full-en rebuild started 2026-09-16 22:03 uses a module built after this change.
- **Report results must be rerun after rebuilding.** That covers the MS MARCO MRR@10 and Recall@1000 figures, the rank-safety table, and the latency tables, since changed bounds change how much gets pruned.

## 3. Action items after PID 40162 exits

PID 40162 is the full-en rebuild (`startorch build-posting --profile full-en`) started 2026-09-16 22:03. Everything below waits for it. Before starting, check it exited cleanly: `ps -p 40162` shows nothing, and `/data/scholar_rank/posting/full_en/posting/metadata.txt` has a fresh timestamp and an `avgdl=` line.

**Status 2026-09-17 00:53: it did not finish.**
- Tokenizing completed: 205 `token_*.bin` files, about 209 GB, the last written at 00:19.
- The doc-length step was partway through. `doc_len_list.bin` was last written at 00:51 and is smaller than the old file.
- `doc_len_meta.bin`, `block_meta.bin`, and `metadata.*` are still the 2026-08-15 versions, so the full-en index folder is inconsistent.
- The kernel log has no out-of-memory kill.
- The token stream is in doc id order, which the new C++ build also accepts, so the three C++ steps could rerun on it without re-tokenizing.

**Claude**

1. Build the `startorch_cpp` module target. It was held back so the rebuild's mapped `.so` isn't overwritten mid-run.
2. Run the spill gate on math-en: the unsorted streaming tokenizer query, with `temp_directory` pointed at an empty folder and `max_temp_directory_size` set to 0 so any spill raises an error, sampling `duckdb_memory()`. It passes if the query completes, the folder stays empty, and temp bytes stay at 0. If it spills, stop and report before editing `tokenizer.py`.
3. Apply the Python half of the tokenizer fix (§2) to `tokenizer.py`.
4. Run the smoke test, `py_compile` on every touched Python file, and the CLI `--help` checks.
5. Run the math-en end-to-end comparison in `python/.tmp`, never `/tmp` (RAM-backed) or `/data`:
   - tokenize with HEAD's `tokenizer.py` and with the new one, then build both with the new C++;
   - these must be byte-identical: `doc_id_lookup.bin`, `doc_len_list.bin`, `posting_*.bin`, `block_meta.bin`;
   - a few queries through `QueryEngine.query` and `query_exhaustive` must return identical results on both;
   - record peak RSS and spill for both tokenizers, then delete the output.

**You**

6. Rename the underscore folders. They are on the same filesystem, so this is instant and copies no data:
   ```
   cd /data/scholar_rank
   mv data/works_subset/full_en data/works_subset/full-en
   mv data/works_subset/math_en data/works_subset/math-en
   mv data/works_subset/engr_en data/works_subset/engr-en
   mv posting/full_en posting/full-en
   mv posting/math_en posting/math-en
   mv posting/engr_en posting/engr-en
   ```

**Claude, after the renames**

7. Run `startorch query --profile full-en --query "..."` to confirm the renamed index loads through the new loader, then grep for leftover underscore profile paths.
8. Record the results of steps 2-7 here.

## 4. Open issues

### Correctness and report accuracy

- **Rank safety is still unverified.** After rebuilding, rerun the BMW-vs-exhaustive check. If missing documents drop to zero, the avgdl mismatch was the cause.
- **The correctness docstring is misleading.** `bmw_correctness.py` says extra documents should be impossible. At a fixed k, every missed document is replaced by the next-best one, so extras always equal misses. The docstring points debugging at the wrong kind of bug.
- **The smoke test restates constants.** It should check behavior instead, and it doesn't cover the `build-posting` or `query` commands.
- **Hand-copied report numbers have drifted.** Fix: render every table from `agg.json` through `render.py`, and cite `full_en_index_stats.json` for corpus facts. Settle the size numbers before the resume goes out, because the draft bullet uses 725 GB and 241 GB.

| Claim | One place says | Another says |
|---|---|---|
| random_set k=10 p99, BMW vs exhaustive | §7.2 table: 11.5 vs 36.2 ms | §8.1 text: 36.2 vs 11.5 ms |
| Optimization level | report: `-O0` | build: `-O3`, forced since Aug 2, two weeks before the first benchmark commit |
| Compact corpus size | README and report: 103 GB | `data_pipeline.md`: 241 GB |
| Raw snapshot size | `data_pipeline.md`: 700 GB+ | report: 780 GB+ |
| Documents indexed | report table: 345,897,793 | mean length of 39.73 was computed over 345,701,167 |

In §8.1, the correct numbers support the paragraph's own point that BMW has a higher floor but a lower ceiling. The swapped numbers undercut it.

### Python duplication

- **Two index-build pipelines.** The MS MARCO build copies the main pipeline, including a forked tokenizer with the same stopword regex and binary format. Root cause: the tokenizer hardcodes OpenAlex SQL. Fix: the tokenizer takes a query that yields id and text, and BM25 parameters move into config, since they already differ per corpus.
- **Five functions that run queries.** Two in `full_en_bench_query.py` differ by one method name, and `ms_marco_pipeline.py` has two more. They now share one engine per run, but each still repeats its own read, tokenize, and loop. Root cause: `retrieve` prints instead of returning, so nothing could reuse it. Fix: a library search function that returns results. The CLI prints them and the benchmarks time them.
- **Path logic in at least six places.** The benchmarks rebuild paths by string concatenation, `utils.py` and `cli.py` each define their own config loader, and `query_gen.py` hardcodes the metadata filename despite the exported constants. Fix: one module that turns a profile name into every artifact path, with MS MARCO as just another profile. That also merges the two config files' overlapping paths.
- **`full-en-2` is an exact copy of `full-en`** in `project-config.toml`.
- **The doc-id lookup format is encoded three times.** The writer, the numpy reader, and the query path's seek arithmetic each hardcode it. Fix: one small class backed by a numpy memmap.
- **Latency stats are computed twice.** Both `benchmarks.ipynb` and `prepare.py` compute them, and the notebook copy already had the p95/p99 bug. Fix: the notebook imports the report's stats function.

### C++ duplication

- **Metadata still stores absolute paths.** `load_index` now ignores them, so renamed indexes load, but the fields and the write-only `.txt` twin are dead weight. Fix: store only parameters and a format version, in the next format change.
- **Two ways to build an index.** Python uses only the bindings, while the CLI apps are the only place k1 and b get validated. BM25 defaults live in four places, and one stage has four names, including a typo in the exported `PostingBuildler` class. Fix: move validation into the library function, and drop the apps unless a Python-free path is wanted.
- **Two full passes over the 210 GB token stream.** Doc lengths and SPIMI construction each read everything, and merge silently depends on a file a separate call wrote into the same folder. Fix: one build entry point that counts doc lengths during the SPIMI pass and enforces the order itself.
- **Smaller items.** VBE decoding is reimplemented for mmap because the VBE utilities only accept a `FILE*`. A buffer-size constant is defined in three files, and the logger's two overloads duplicate each other.

### Documentation

- **Status is tracked in three places:** README, `algorithm_design.md`, and `CLAUDE.md`. The test count reads 134 in some and 154 in others; the real count is now 169. `algorithm_design.md` and `retrieval_engine.md` still say queries can't run outside C++.
- **BMW design is explained in four documents:** `retrieval_engine.md`, `BMW_writeup.md`, report §3, and the README.
- **Stale references.** `CLAUDE.md` still points at the deleted `BMW_benchmarks.md` in four places. The notebook README lists a deleted notebook, and the benchmark README is empty.
- **Fix:** one home per fact. The README owns status, `retrieval_engine.md` owns design, and the report owns experiments and links to the design instead of restating it.

## 5. Priority order

The full-en rebuild is already running with the current format. Further format changes (dropping stored paths, a format version, a single-pass build) would force another full rebuild, so they should be batched into one.

1. **Finish §3** once the rebuild exits, including the tokenizer's Python half and the folder renames.
2. **Rebuild MS MARCO,** since it is small, to check the pipeline end to end and rerun the MS MARCO quality numbers.
3. **Rerun the rank-safety check and latency benchmarks** on the new full-en index.
4. **Fix the report numbers** by rendering every table from `agg.json`, before sharing the report or the resume.
5. **Add the path module and a search function that returns results.** Most of the remaining Python duplication disappears once both exist.
6. **Batch the remaining format changes** (drop stored paths, add a format version, single-pass build) into the next rebuild.
7. **Consolidate the docs.**
