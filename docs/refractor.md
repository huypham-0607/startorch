# Startorch refactor plan

Last updated 2026-09-19. Refactor round 2 is done and verified (§2). **Next: rebuild MS MARCO and full-en with the new pipeline (§3).** Tracks the refactor that came out of the full-repo design review: how the pipeline works today, what has been fixed, and what is left.

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
| Build | Python gives every document a dense integer id, builds its text from title, topic hierarchy, and keywords, strips stopwords, stems, and writes a binary token stream. C++ `build_index` then reads the stream once: SPIMI builds partial indexes under a memory cap while counting document lengths, and the merge combines them. The C++ steps accept documents in any doc id order. The merge computes avgdl and each block's maximum possible BM25 score. | `doc_id_lookup.bin`, `token_*.bin`, `doc_len_list.bin`, `posting_*.bin`, `block_meta.bin`, `metadata.bin` / `.txt` |
| Query | Tokenizes the query with the same rules. `QueryEngine` loads metadata (including avgdl), document lengths, and block metadata into memory, memory-maps the posting files, and runs Block-Max WAND. Python maps the returned ids back to OpenAlex ids and prints them. | stdout |

Each CLI `query` call is a new process, so it still pays the full index load, about 96 s on full-en. The benchmarks keep one engine for a whole query set.

Benchmarking and reporting live outside the CLI:

- `query_gen.py` generates synthetic query sets from term document frequencies.
- `bmw_performance.py` and `bmw_correctness.py` run those sets through the pruned and exhaustive engines, appending latency, memory, and result comparisons to Parquet files.
- MS MARCO is the `msmarco` profile. It builds through the same pipeline, and `bmw_correctness.py ms-marco` writes run files so result quality can be scored against Anserini.
- `report/prepare.py` combines the Parquet files into `agg.json`, and `report/render.py` renders tables. Those numbers are currently copied by hand into `bmw_technical_report.md`.

All configuration lives in `project-config.toml`. `startorch.paths.profile(name)` turns a profile name into every path and build parameter. All data lives under `/data`, outside the repo.

## 2. Done

### 2026-09-19: refactor round 2 (done and verified)

Plan: `refractor_scratch.md`, checked item by item for feasibility and approved 2026-09-19. One idea was not feasible: a 64 MB buffer on the stack. The stack limit is 8 MB (`ulimit -s`), pybind11 runs on Python's main thread, and the merge holds one reader per partial block (234 on full-en). So buffers are allocated once on the heap, with a fixed documented default.

**Why the I/O work.** The drive reads 4.7 GB/s (O_DIRECT), but every build reader went through `FILE*` with 4 KB buffers and several locked stdio calls per record:
- `read_token` did three `fread`s plus an `ftell`;
- `read_vbe` did one `fread` per byte;
- `SafeFile::fread` called `ftell` before every read.

A full-en token pass spent about 17 minutes in stdio calls, and the build made two passes. The aborted 2026-09-16 rebuild's doc-length pass ran at about 120 MB/s.

#### C++: done, 193 of 193 tests pass (up from 169)

- **C1/C2 `BufferedReader`** (`file_io`):
  - one `read()` per refill;
  - `read(value)` for one value and `fread(dst, n, count = 1)` for `count` consecutive items, with an inline fast path, plus in-buffer VBE decoding (`fread` was `read_bytes(dst, n)` until the API was unified with `BufferedWriter`, below);
  - reads return false at a clean end of file and throw on a truncated one, with no `ftell`;
  - `posix_fadvise(SEQUENTIAL)`;
  - a 64 MiB default, and 4 MiB per merge stream.

  Ported to it: `read_token`, the merge's `Stream`, `read_doc_len_list`, `read_block_meta_file`, `read_metadata`. `SafeFile::fread` counts bytes instead of calling `ftell`. The legacy `SafeFile`/`FILE*` overloads stay, so every old test passes unchanged, and parity tests check the old and new readers return identical records.
- **C3 metadata format 2:**
  - `metadata.bin` holds the magic bytes `STMD`, version 2, then k1, b, avgdl, block_size, split_size, and no paths;
  - `metadata.txt` keeps the parameters plus `format_version`;
  - a format 1 file fails with "...Rebuild the index."
- **C4 `BufferedWriter`:** writes through the file descriptor, with one `memcpy` per call, a 64 MiB default, and calls larger than the buffer bypassing it. The three `BUF_SIZE` copies are gone.
  - Its API mirrors the reader's: `write(value)` for one value and `fwrite(src, n, count)` for `count` consecutive items. `write` takes only lvalues, so a value's width on disk is always its declared type; `write(term.size())` fails to compile rather than writing 8 bytes. Every single-value `fwrite(&x, sizeof(x), 1)` in the build is now `write(x)`, which writes the same bytes. 196 of 196 tests pass, with three new ones: a write/read round trip, a multi-item `fread` across refills, and a truncated multi-item read.
- **C5 `build_index(token_dir, partial_dir, out_dir, params)`:**
  - SPIMI counts document lengths as it reads (`DocLenCounter`), so the token stream is read once;
  - the doc-length files are then written, and the merge core, `merge_partial_blocks`, takes the lengths as arguments;
  - `merge_inverted_blocks` stays as a wrapper that loads them from files, for the app and the tests;
  - leftover `block_*.bin` files are removed first.
- **C6 `BuildParams`** (`build_params.h`): the only home of the defaults (k1 1.2, b 0.75, block 128, split 1 GiB, SPIMI memory 1 GiB). `validate()` requires k1 in (0, 5], b in [0, 1], and every size > 0.
  - The merge app used to document k1 in [1, 2] while its code allowed [0.1, 5.0]; MS MARCO's 0.82 now passes one shared check.
  - `calc_BM25` no longer has defaults.
  - The bindings expose only `build_index` for building, with keyword defaults read from `BuildParams`.
- **C7 per-record CPU:**
  - SPIMI does one `try_emplace` per token, where it used three lookups;
  - `write_partial_index` looks each term up once;
  - the merge checks "still the same term?" by comparing list ids, instead of copying and comparing a string per posting.
- **C8 smaller items:**
  - `vbe_decode_from`, shared by `BufferedReader` and the query path, which also decodes straight from the mmap now;
  - one `Logger` constructor;
  - `cpp/benchmarks/io_bench.cpp` (CMake target `io_bench`).

**Reader cost** (`io_bench`, warm page cache, real files; the legacy path also got faster from losing its `ftell`):

| Reader | 2026-09-17 | Legacy path now | `BufferedReader` |
|---|---|---|---|
| Token | 75 ns | 24 ns | 13 ns |
| Partial-block posting | 66 ns | 14 ns | 3 ns |
| Doc-length entry | 69 ns | 65 ns | 6 ns |
| Block metadata, per block | 362 ns | — | 62 ns |

**Build equivalence.** `build_index` was run over the pre-round-2 baseline token streams and gives byte-identical `posting_*.bin`, `block_meta.bin`, `doc_len_list.bin`, and `doc_len_meta.bin`, with matching parameters:

| Index | Old 3 steps (doc len + SPIMI + merge) | `build_index` |
|---|---|---|
| math-en | 81.7 s (24.0 + 39.3 + 18.4) | 21.8 s |
| MS MARCO | 152.3 s (40.7 + 70.2 + 41.4) | 37.2 s |

#### Python

- **`project-config.toml`:** one `[profiles.<name>]` table per profile (`kind`, `filter`, `materialize`, optional k1/b/block-size/split-size/mem-limit), plus `[paths]`, `[folders]`, `[files]`.
  - `full-en` has `materialize = false`; the small OpenAlex profiles are copied.
  - MS MARCO is the `msmarco` profile, and its index root stays `/data/scholar_rank/benchmark/msmarco`.
  - `full-en-2` is deleted, and so is `benchmark-config.toml`.
- **`startorch/paths.py`:** a cached `load_config()`, `profile(name)` returning a frozen `Profile` (every path, `meta_path`, and `build_params`), plus `profile_names(kind)` and `corpus_paths()`. The old loaders in `utils.py` are gone.
- **`startorch/doc_id_lookup.py`:**
  - `RECORD` (the only layout definition);
  - `lookup_records`;
  - `map_raw_ids`, which raises if an id is missing;
  - `DocIdLookup`, backed by a memmap, with `raw_ids()` and `mapped_ids()`.

  `Tokenizer.read_doc_id_lookup` is deleted.
- **`tokenizer.py`:**
  - `token_expression(text_sql)` is shared by documents and `tokenize_query`;
  - the source queries are `openalex_source(glob, condition)`, `msmarco_source(collection)`, and `profile_source(profile)`;
  - `get_token(source_sql, out_path, lookup_file, spill_path)` removes leftover `token_*.bin` files first;
  - `close()` releases DuckDB;
  - `TokenizerMSMARCO` is gone. The forked stopword regex was byte-identical, and MS MARCO pids are exactly 0 to 8,841,822, so its lookup is the identity mapping.
- **`works_subset.py`:**
  - `count_matching()` is new;
  - the `COPY` runs with `preserve_insertion_order = false`;
  - validation is one subset scan (count, distinct BIGINT ids, filter mismatches) plus a filtered corpus count;
  - the link-count and dangling-link lines are dropped;
  - `main()` validates a copied profile.
- **`build_posting.py`:** `PostingBuilder(profile)` (typo fixed) runs the tokenizer, closes it, then calls `startorch_cpp.build_index(..., **profile.build_params)`.
- **`search.py`:**
  - `Searcher(profile)` loads the engine once and records `load_latency`;
  - `search`, `search_terms`, and `search_many` return `SearchResult(hits=[(raw_id, score)], elapsed)`;
  - `read_query_file(path, cap)` parses query sets.
- **`cli.py`** keeps only parsing and dispatch through `profile()`. `gen-works-subset` offers OpenAlex profiles and only counts ones that aren't copied. `build-posting` and `query` offer every profile.
- **`__init__.py`** exports the above.
- **Deleted:** `query_wrapper/`, `benchmark/retrieval/full_en_bench_query.py`, `benchmark/retrieval/ms_marco_pipeline.py`.
- **Ported callers:**
  - `bmw_performance.py` now has one `run_queries(profile, filename, k, engine, cap)` over `Searcher`, with the same Parquet columns and run names. MS MARCO still builds its index first if it's missing.
  - `bmw_correctness.py`'s BMW-vs-exhaustive check loads one engine for both passes, where it used to load the index twice. It records OpenAlex ids, and the run files take `(pid, score)` hits.
  - `query_gen.py`, `report/prepare.py`, and `benchmarks.ipynb` take every path from `profile()`.
  - `smoke_test.py` gained six checks: `build-posting` and `query` `--help`, `--profile` choices per command, golden paths, `build_index` rejecting bad parameters before reading anything, and a `DocIdLookup` round-trip.

#### Verification (2026-09-19)

- **Compile, import, smoke test:** all 17 Python files compile, the benchmark modules import, and the smoke test passes 20 of 20. `profile()` resolves every index, query set, and results folder to where the data already lives; the one intended change is that MS MARCO shares the main spill folder.
- **math-en, `PostingBuilder` end to end:** byte-identical to the pre-round-2 baseline, covering `doc_id_lookup.bin`, `doc_len_list.bin`, `doc_len_meta.bin`, `block_meta.bin`, and `posting_0000.bin`, with matching parameters.
  - All 62 fixed queries at k = 10 and 1000, pruned and exhaustive, return results identical to the baseline.
  - `Searcher`'s OpenAlex ids match the old `retrieve` seek mapping, 40 of 40.
- **MS MARCO, shared tokenizer plus `build_index`:** byte-identical to the forked-tokenizer baseline, all five index files. The new lookup maps each of the 8,841,823 pids to itself.
  - All 6,980 dev queries at k = 1000 return identical results, as do 200 of 200 exhaustive checks.
  - MRR@10 is 0.19248 and Recall@1000 is 0.8778. The index on `/data` scored 0.19257 and 0.8778, and Anserini 0.1892 and 0.8573. The small MRR shift comes from round 1's IDF change, since this round's index is byte-identical to the baseline.
- **full-en lookup read from the corpus through `language = 'en'`,** with no subset copy: byte-identical to the existing `/data/.../full-en/lookup/doc_id_lookup.bin`. It covers 345,897,793 ids, took 30 s, peaked at 8.8 GB RSS, and didn't spill.
- **works_subset:**
  - math-en and engr-en were regenerated into `python/.tmp`. Row counts match and the id sets are identical in both directions.
  - The new validation reports 0 errors, and engr-en's counts match its 2026-08-14 log (33,646,156 nodes, 328,123,750 links).
  - Validation took 29.0 s on math-en and 32.9 s on engr-en.
  - The existing math-en log, written by the old per-row validator, lists thousands of "Duplicate ID" lines. Those are wrong: the lookup build requires strictly increasing ids and passed on math-en, and the new validation finds 0 duplicates.
  - **`preserve_insertion_order = false` on the `COPY` didn't speed it up:** engr-en took 212.8 s with it off and 215.6 s with it on. The copy is bound by scanning the corpus. It stays off, since it's harmless.
- **Leftover grep:** clean, apart from the `METADATA_BIN` constant itself and a test fixture that deliberately writes a format 1 path.

**Timing, old against new pipeline:**

| | Tokenize | Index build | Index load |
|---|---|---|---|
| math-en | 49.9 → 46.3 s | 81.7 → 17.9 s | 1.17 → 0.15 s |
| MS MARCO | 105.9 → 77.0 s (fork, 10.4 GB peak RSS) | 152.3 → 36.3 s | 2.91 → 0.43 s |

**Behavior changes to remember:**
- **Every index needs a rebuild** (metadata format 2); none had been rebuilt anyway.
- **Stale files are removed:** `build_index` deletes stale partial blocks and the tokenizer deletes stale token files. Before, a longer earlier run's extra files were silently merged in.
- **Correctness results** will record OpenAlex ids instead of internal mapped ids; for MS MARCO the two are the same.
- **One spill folder**, `[paths] spill`; MS MARCO used to have its own.

### 2026-09-17: refactor round 1

Five items from `refractor_scratch.md`. None of them changes the on-disk index format (round 2's metadata format 2 does). Some verification waits for the next full-en build; see §3.2.

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
- Run from Python on 2026-09-17, after the module rebuild: both math-en test indexes answered 64 query/k pairs through `QueryEngine`.

#### Tokenizer memory fix, Option 3

**Problem.** To hand C++ a stream in doc id order, the tokenizer joins every document to its dense id, sorts every document's token list, and materializes every token as a row in a DuckDB temp table: about 13.7B rows on full-en. Under a 512 MB DuckDB memory cap, this ran out of memory on the full math-en subset. A sorted Arrow stream with no temp table completed under the same cap, but still spilled about 1.3 GB for its sort.

**Decision.** Keep corpus file order and make the C++ build accept any doc order.

**C++ (done).**

- `write_partial_index` sorts each term's posting list by doc id before writing gaps, merging repeated doc ids (`PostingList::sort`). This happens in memory, within the SPIMI cap, and is skipped for lists that are already increasing.
- `construct_doc_len_list` counts tokens into an array indexed by doc id, then writes the non-zero entries in doc id order. That is about 1.4 GB on full-en, the same array the merge and the query engine already hold. The earlier estimate that the doc-length file format would change was wrong; it is unchanged.
- `build_posting_list` is unchanged. Its heap merge already accepts partial blocks whose doc id ranges overlap, and already merges a document split across two partial blocks. Sorting there would have held a whole posting list in memory, about 5.5 GB for the most common full-en terms.
- A stream already in doc id order builds exactly as before. That covers the MS MARCO tokenizer fork and token streams written by the old tokenizer.

**Python (done 2026-09-17, after a spill gate passed on math-en: the unsorted streaming query read all 3,794,495 documents with DuckDB spill disabled, at 2 GB and 4 GB caps with 20 threads, and at 1 GB with 4 threads).** Changes to `tokenizer.py`:

- `build_doc_id_lookup` sorts only the raw ids and runs once, where it used to run twice. It writes the lookup file one numpy batch at a time, with the same bytes as before, and returns the sorted raw ids (about 2.8 GB on full-en). It raises unless those ids are strictly increasing, which also rules out duplicates.
- `get_token` streams `(raw id, tokens)` batches of 50,000 documents with no ORDER BY, JOIN, or temp table. It maps raw ids to dense ids with `np.searchsorted` and raises if any id is missing from the lookup.
- **`preserve_insertion_order` is off for the token query.** With it on, DuckDB must hand batches over in file order, so the parallel scan waits on its slowest thread: 134 s against 37 s on math-en. The C++ build accepts any order, so token files now differ from run to run while the index stays byte-identical. The lookup's ORDER BY is unaffected.
- `encode_token_records` packs a whole batch with numpy and pyarrow, replacing the per-token `struct.pack` loop. A new token file starts after `row_per_chunk * chunk_per_file` tokens (2^26, about 1.1 GB), as before.

**Measured on math-en** (3,794,495 documents, 183.9M tokens), with the same C++ build for both tokenizers:

| | HEAD tokenizer | New tokenizer |
|---|---|---|
| Tokenize | 124 s | 50 s |
| Peak RSS while tokenizing | 6.1 GB | 2.0 GB |
| DuckDB spill | 0 | 0 |
| Doc lengths / SPIMI / merge | 44 / 67 / 33 s | 45 / 77 / 36 s |

- `doc_id_lookup.bin`, `doc_len_list.bin`, `doc_len_meta.bin`, `block_meta.bin`, and `posting_0000.bin` are byte-identical between the two builds, and the metadata parameters match. The token streams are the same size (3,031,011,837 bytes) but in a different order.
- 32 queries at k = 10 and 1000 (common, mid-frequency, rare, unindexed, and empty) return identical results on both indexes through the new Python `QueryEngine`.
- SPIMI is about 10 s slower on the unsorted stream, which fits sorting each term's list before writing it.
- **Remaining streaming cost.** The same query computed fully in parallel takes 11 s, so about 26 s of the 37 s is spent outside the parallel scan, probably in single-threaded Arrow conversion. Estimated full-en streaming time: about 46 minutes, against about 30 minutes for the old join, sort, and temp table plus 104 minutes of per-token writing.
- **Memory.** Streaming needs about 1 GB of unspillable DuckDB working memory at 20 threads. It fails under a 1 GB cap and passes at 2 GB or with 4 threads. The tokenizer uses DuckDB's default cap (80% of RAM), so this only matters if a cap is set.

#### Profile spelling

- A profile's name is its folder name everywhere. The translation function `get_subset_folder` is deleted, and the `full_en` path literals in `works_subset.py`, `full_en_bench_query.py`, `query_gen.py`, and `benchmarks.ipynb` now say `full-en`.
- Python module and identifier names such as `full_en_bench_query.py` keep underscores, because modules can't be imported with dashes. Data file names such as `full_en_index_stats.json` are not profile paths and keep theirs too.
- The six `/data` folders were renamed to match on 2026-09-17: `works_subset/` and `posting/` now hold `full-en`, `math-en`, and `engr-en`.

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

- **Every existing index under `/data` must be rebuilt.** The metadata format changed, and old files now fail to load with "Failed to read file" (verified on math-en). The stored block bounds also depend on the IDF formula. The full-en rebuild started 2026-09-16 22:03 was stopped before it finished (§3), so no index has been rebuilt yet.
- **Report results must be rerun after rebuilding.** That covers the MS MARCO MRR@10 and Recall@1000 figures, the rank-safety table, and the latency tables, since changed bounds change how much gets pruned.

## 3. Next action items

Refactor round 2 is done (§2). The verification scratch space `python/.tmp/refactor2` was deleted once its results were recorded above.

1. **You:** rebuild MS MARCO, then full-en, with `startorch build-posting --profile <name>`.
   - Every index under `/data` still has format 1 metadata, so each fails to load until it is rebuilt.
   - The full-en index folder is also inconsistent from the build stopped on 2026-09-16.
   - Expected on full-en: tokenizing is about 2.5× faster than the old tokenizer at about a third of its memory, and it reads the corpus through `language = 'en'` directly (the 79 GB `works_subset/full-en` copy is no longer read).
   - The C++ build reads the token stream once, with readers 3 to 20 times cheaper per record.
2. **Claude, after the rebuilds:**
   - run `startorch query --profile full-en --query "..."`;
   - rerun the rank-safety check and latency benchmarks, and the MS MARCO quality numbers (`bmw_correctness.py ms-marco`);
   - record the results here.
3. **You, optional:** once the full-en rebuild succeeds, `works_subset/full-en` (79 GB) is unused and can be deleted.

## 4. Open issues

### Correctness and report accuracy

- **Rank safety is still unverified.** After rebuilding, rerun the BMW-vs-exhaustive check. If missing documents drop to zero, the avgdl mismatch was the cause.
- **BMW and exhaustive scores can differ in the last float bit, so exact list equality is too strict.** On math-en, 1 of 64 query/k pairs differed: the same 1,000 documents in the same order, but one document scored 7.766792297 in BMW and 7.766792774 exhaustively, 1 ULP apart. `evaluate_prefix` sums term contributions in the order `std::sort` leaves tied cursors, and `std::sort` isn't stable, so the two search paths add the same floats in different orders. `bmw_correctness.py`'s `rank_matches` compares lists exactly, so it will count this as a mismatch, while its missing and extra document counts stay correct. Fix: make the order within a tied run deterministic (for example, break doc id ties by term), or compare scores with a tolerance.
- **The correctness docstring is misleading.** `bmw_correctness.py` says extra documents should be impossible. At a fixed k, every missed document is replaced by the next-best one, so extras always equal misses. The docstring points debugging at the wrong kind of bug.
- **The smoke test is still mostly shallow.** Round 2 added behavior checks (golden paths, `--profile` choices, `build_index` parameter validation, `DocIdLookup` round-trip), but it still never builds or queries a real index. A small fixture index would cover `build-posting` and `query` end to end.
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

- **Done in round 2 (§2):** two index-build pipelines (shared tokenizer, MS MARCO as a profile), five functions that run queries (`Searcher`), path logic in six places (`paths.py`, one config file), `full-en-2`, and the doc-id lookup format encoded three times (`doc_id_lookup.py`).
- **Latency stats are computed twice.** Both `benchmarks.ipynb` and `prepare.py` compute them, and the notebook copy already had the p95/p99 bug. Fix: the notebook imports the report's stats function.

### C++ duplication

- **All done in round 2 (§2):** stored metadata paths, two ways to build an index (validation now in `BuildParams`; the apps are kept), two passes over the token stream (`build_index`), and the smaller items.
- **Still open:** the `PostingPointer` constructor looks terms up with `term_meta_mapping[term]`. On an `unordered_map`, that isn't safe for concurrent readers. Switch to `find()` before the engine serves queries from several threads.

### Documentation

- **Status is tracked in three places:** README, `algorithm_design.md`, and `CLAUDE.md`. The test count reads 134 in some and 154 in others; the real count is now 193. `algorithm_design.md` and `retrieval_engine.md` still say queries can't run outside C++.
- **BMW design is explained in four documents:** `retrieval_engine.md`, `BMW_writeup.md`, report §3, and the README.
- **Stale references.** `CLAUDE.md` still points at the deleted `BMW_benchmarks.md` in four places. The notebook README lists a deleted notebook, and the benchmark README is empty.
- **Fix:** one home per fact. The README owns status, `retrieval_engine.md` owns design, and the report owns experiments and links to the design instead of restating it.

## 5. Priority order

1. **Rebuild MS MARCO, then full-en** (§3), and rerun the MS MARCO quality numbers, the rank-safety check, and the latency benchmarks.
2. **Fix the report numbers** by rendering every table from `agg.json`, before sharing the report or the resume.
3. **Remaining open issues (§4):**
   - the latency stats duplicated between `prepare.py` and the notebook (`prepare.py` must stop doing its work at import time first);
   - the 1-ULP score comparison and the correctness docstring;
   - a fixture-index smoke test;
   - the engine's `operator[]` lookup;
   - consolidating the docs.
