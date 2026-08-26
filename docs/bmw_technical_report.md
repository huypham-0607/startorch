# Block-Max WAND BM25 IR Technical & Benchmark Report
## 1. Abstract

This document reports the technical design & benchmark results for an implementation of Block-Max WAND with BM25 scoring engine, which is used as a High-Recall retrieval engine for the Startorch project.

Benchmarks include:
- MRR@10 and Recall@1000 against Anserini BM25 implementation
- MS-MARCO retrieval latency
- OpenAlex retrieval latency.

### Quick summary

#### MS MARCO Retrieval Quality

Compared Startorch BMW MRR@10 and Recall@1000 performance with MS MARCO Passage Dataset against Anserini BM25. Results show similar performance between the two implementations.

| MS MARCO Metrics       | Startorch BMW | Anserini BM25      |
| ---------------------- | ------------- | ------------------ |
| `MRR@10`               | 0.1926        | 0.1892             |
| `Recall@1000`          | 0.8778        | 0.8573             |

#### MS MARCO Retrieval Latency

| Queryset               | n       | Mean (ms) | p50 (ms) | p95 (ms) | p99 (ms) |
| ---------------------- | ------- | --------- | -------- | -------- | -------- |
| `queries.dev.small.tsv`| 6980    | 16.020    | 10.645   | 47.201   | 85.465   |

#### OpenAlex Retrieval Latency

These are all synthetically generated short query sets mimicking realistic load in production. Each short query set consists of 2000 queries with 3-4 distinct terms registered in posting list. They are classified into 5 types:
- Random set: randomly sampled terms.
- Common set: **top 1%** highest document frequency.
- Very common: **top 0.01%** highest document frequency.
- Rare set: **bottom 10%** highest document frequency.
- Skewed set: **top & bottom 0.01%** highest document frequency.

| name                                |    n |   max_rss (MB) |     mean |      p50 |       p95 |       p99 |       max |     min |
|:------------------------------------|-----:|----------:|---------:|---------:|----------:|----------:|----------:|--------:|
| `short_random_set.tsv_10`             | 2000 |   11404.4 |    0.493 |    0.015 |     2.027 |     3.569 |     8.505 |   0.002 |
| `short_common_set.tsv_10`             | 2000 |   11400.3 |    2.021 |    0.736 |     5.251 |    20.658 |   259.693 |   0.072 |
| `short_very_common_set.tsv_10`        | 2000 |   19223.6 |  218.796 |  185.421 |   444.786 |   707.867 |  3780.53  |   2.469 |
| `short_rare_set.tsv_10`               | 2000 |   11400.2 |    0.877 |    0.017 |     3.149 |     4.82  |    11.149 |   0.002 |
| `short_skewed_set.tsv_10`             | 2000 |   17547.1 |   89.069 |   50.138 |   302.425 |   534.029 |  4531.3   |   0.003 |


## 2. Introduction and scope

BM25 has existed since the 90s, and is still one of the most popular lexical retrieval baseline for most retrieval tasks. For Startorch, BM25 is designed as a lightweight IR engine to quickly retrieve relevant documents, which will then be accurately re-ranked using other algorithms.

Originally, we wanted to use DuckDB built in BM25 in the FTS module. But since Startorch operates on hundreds of millions of documents, an exhaustive disjunctive top-k BM25 implementation would be too slow. Hence, we decided to implement our own Safe Block-Max WAND BM25 variant (Ding & Suel, 2011).

This report will answer five main questions:
1. How was the entire IR pipeline designed?
2. Does BM25 implementation matches a pre-existing reference?
3. Is rank-safety established by Block-Max WAND optimization?
4. How does Block-Max WAND perform at 345M OpenAlex document scale? How does it compare to exhaustive search?
5. How does Block-Max WAND latency respond to term document-frequency?

*PS. I think this report best-serves as an experimentation on Block-Max WAND rather than a research artifact. It's best to treat this as a technical report for a system engineering project.*

## 3. Retrieval Design & Implementation

This section will go over some implementation details of our BMW BM25.

### OpenAlex Corpus

Our main corpus for this project is OpenAlex Works entity, which is an open-source academic catalog, featuring hundreds over 510 million scholarly works. For this project, we will only be working with subset of works written in English (which is around 345 million works).

Data are fetched directly from OpenAlex S3 storage. Since we are working with 780 GB+ of compressed data, we've designed our pipeline as follows:

1. Download one chunk of raw data.
2. Strip away fields irrelevant for this project. See remaining fields at `data_reference.md`
3. Validate per-chunk integrity.
4. Delete the raw chunk, keep only the compact version.

This keeps disk usage manageable throughout the process. Compact data is stored as [Parquet](https://parquet.apache.org/), and has cumulative size of 103GB compressed. Source code for fetching process can be found at `ingest/fetch_data.py`

### Tokenization

For the scope of this project, **we will not be indexing full documents**. There are a few reasons for this decision, though here are the main ones

- Space constraint on device (1 TB of storage).
- A substantial percentage of documents are not open-access.
- Complexity of processing and handling documents, as well as crawling process if needed, is unfeasible workload for a solo project, and is not the core objective of Startorch.

Alternatively, we will construct our document by concatenating elements across 6 fields

| Field name                | Type          | Desc                                                              |
| ------------------------- | ------------- | ------------------------------------------------------------------|
| `title`                   | `VARCHAR`     | Title of given work |
| `topics`                  | `VARCHAR`     | OpenAlex assigned topics of given work |
| `subfields`               | `VARCHAR`     | OpenAlex assigned subfields of given work |
| `fields`                  | `VARCHAR`     | OpenAlex assigned fields of given work |
| `domains`                 | `VARCHAR`     | OpenAlex assigned domains of given work |
| `keywords`                | `VARCHAR`     | OpenAlex assigned keywords of given work |
 
Readers can reference OpenAlex documentation on how they determine topics and keywords.

Currently, these fields are treated as one flat bag of word model (no field weighting yet). This is worth looking into in retrospect as certain fields (eg. `title`) has higher indicative value than a field like `domain` (Though this is partly remedied by BM25 term saturation).

An issue worth flagging, OpenAlex treat topics as a hierarchy of `domains` &rarr;`fields` &rarr; `subfields` &rarr; `topics`. A consolidated research paper might have list of `topics` that share common ancestors at higher level in the hierarchy. This has the potential to skew both BM25 accuracy and BMW pruning effectiveness for common terms.

Tokenization step will be implemented in Python (`tokenizer/tokenizer.py`). Results will be serialized and saved as binary shards `token_*.bin`

We will consider each single word as a token. Hyphen connected words are collapsed an considered a single word. For instance "We value your well-being" will be considered 5 tokens: "We", "value", "your", "well", "being". For the scope of this project, machine learning based tokenization methods are not considered.

Normalization rules are as follows:
- All accents are stripped.
- All tokens will be decapitalized.
- All non-alphabetical & non-numerical characters are stripped (This is of minimal impact for our dataset, since our fields are generally low-nuance).
- Stopwords are stripped from the concatenated document text *before* tokenization (a fixed English stopword list, matched with regex word boundaries, replaced globally — not just the first occurrence).

For stemmer, we use DuckDB FTS's built-in `stem(token, 'english')` function directly in SQL, which  is Snowball/Porter2 under the hood.

### Inverted index list (Posting list)

During the tokenization process, Python will also produce another artifact - a remapping of OpenAlex Works id (saved as `doc_id_lookup.bin`). Raw OpenAlex IDs are sparse and exceed `int32`, so documents are squished into `[0,N)` space. There are two main benefits to this.

- Reduced disk usage for doc_id storage from 8 bytes to 4 bytes.
- Any doc_id mapping downstream can be referenced with flat array instead of a hashmap.

Everything downstream from now on will be written in C++.

**Build pipeline, each with their own file** - (`cpp/src/retrieval/`):

| Stage | File | Output |
|---|---|---|
| SPIMI partial-block construction | `construct_inverted_blocks.cpp` | `block_*.bin`, one set per memory-limited chunk |
| Document length table | `construct_doc_len_list.cpp` | `doc_len_list.bin`, dense array by doc_id |
| K-way merge + BMW block-metadata | `merge_inverted_blocks.cpp` | `posting_*.bin` + one consolidated `block_meta.bin` |

Shared pieces both later stages depend on:
- `posting_list.cpp` (`PostingItem`/`PostingList`)
- `bm25.cpp` (`calc_BM25`/`bm25_saturation`)
- `token_stream.cpp` (`read_token`, the tokenizer wire-format reader)
- `vbe.cpp` (Variable byte encoding for doc_id, expensive saving as `unsigned long long`).

This is a classic SPIMI out of core indexing setup. Token are read and accumulate in memory, and flushes when memory usage is reaching certain threshold. This results in several "blocks" of posting lists. These blocks are then merged by maintaining a read pointer for each block, relying on lexicographically sorted term invariant to produce complete posting lists in one pass.

`merge_inverted_blocks.cpp` also produces `block_meta.bin`, which saves block level information (start, end position in file, block-level term contribution) necessary for BMW pruning.

An additional document length table is computed to facilitate exact BM25 computation for each document. Technically, we can store exact BM25 score directly in posting list. But we decided to store term frequency instead to avoid recomputation if design changes.

### Index characterization

| Quantity | Value |
|---|---|
| Documents indexed (English works) | 345,897,793 |
| Unique terms (post stopword removal + Porter2 stemming) | 24,698,967 |
| Total postings | 8,960,802,200 |
| Total blocks (128 postings/block) | 93,937,047 |
| Mean document length (tokens) | 39.73 |

**On-disk artifacts**

| Artifact | Size | Loaded at startup? |
|---|---|---|
| `posting_*.bin` (VBE compressed) | 44.4 GB | mmap, on demand |
| `block_meta.bin` | 2.1 GB | Yes, fully resident |
| `doc_len_list.bin` | 1.6 GB | Yes, fully resident |
| `doc_id_lookup.bin` | 3.9 GB | Offload to python side |
| **Total** | 52 GB | |

**Posting list length distribution**

| Percentile | Document frequency |
|---|---|
| min | 1 |
| p50 | 1 |
| p90 | 4 |
| p99 | 233 |
| p99.99 | 364,624 |
| max | 272,086,781 |

### Query Engine

This is where we get substantial performance gain on exhaustive top-k BM25. The key idea for any WAND-based methods is to skip ahead any documents whose score will never exceed a certain threshold, estimated by their term maximum contribution across all documents.

Block-Max WAND improved upon this by chunking posting lists into blocks (usually of size 64 or 128), and using max term contribution across documents inside that block, enforcing an even stricter estimation.

Readers can read more about Block-Max WAND in the original research paper.

The query engine is implemented in `src/retrieval/query_engine.cpp`. Here is quick implementation rundown.

First, we load all posting & block metadata into memory. Then, each query term gets its own cursor (`PostingPointer`) into that term's posting list. This is implemented by traversing a memory mapped file (for efficiency with non-sequential reads). On every round, the engine sorts the cursors by the document ID, calculating pivot (position where running global WAND score exceeded current top-k threshold). Two optimizations compared to exhaustive search:

- **Term-level optimization**: If an best possible BM25 score of every document term cannot make it to top-k, skip all documents from beginning of cursor list to the pivot.
- **Block-level shortcut**: If block level BM25 score does not exceed top-k threshold, skip the whole block without decoding a single posting inside it.

Only documents surviving both pruning optimizations ever got exact BM25 score computed.

All block level operations are done **in-memory**, and the algorithm is designed to minimize the number of document level access as much as possible, as long as pruning as many uncompetitive documents as possible. This keeps query latency low.


## 4. Experimental setup

### Environment setup

| Item | Description |
|---|---|
| Processor | Intel Core Ultra 7 Processor 255HX  |
| RAM | Samsung 16x2 GB DDR5 5600 MT/s |
| Storage | Western Digital SN7100 1TB NVMe SSD Gen4 PCIe |
| Operating System | Arch Linux x86_64 |
| Kernel version | Linux 7.1.8 |
| Compiler | g++ (GCC) 16.2.1 |
| Language standards | `-std=c++20` |
| Optimization flags | -O0 |
| BMW block size | 128 |

### OpenAlex query sets

For OpenAlex corpus, query sets are synthetically generated by randomly sampling terms from present in posting lists. Each query set also has additional generation requirements to test extreme/adversarial cases.

| Queryset | No of queries | Query length | Sampling method |
|---|---|---|---|
| `random_set.tsv` | 2000 | [5,10] | Uniform random sampling. |
| `common_set.tsv` | 2000 | [5,10] | Random terms with top 1% highest doc frequency. |
| `very_common_set.tsv` | 2000 | [5,10] | Random terms with top 0.01% doc frequency. |
| `rare_set.tsv` | 2000 | [5,10] | Random terms with bottom 10% lowest df. |
| `skewed_set.tsv` | 2000 | [5,10] | Random terms with top or bottom 0.01% highest df |
| `short_random_set.tsv` | 2000 | [3,4] | Uniform random sampling. |
| `short_common_set.tsv` | 2000 | [3,4] | Random terms with top 1% highest doc frequency. |
| `short_very_common_set.tsv` | 2000 | [3,4] | Random terms with top 0.01% doc frequency. |
| `short_rare_set.tsv` | 2000 | [3,4] | Random terms with bottom 10% lowest df. |
| `short_skewed_set.tsv` | 2000 | [3,4] | Random terms with top or bottom 0.01% highest df |

### MS MARCO Passage Full Ranking query set

MS MARCO Passage Ranking query set is useful for comparing Startorch BM25 retrieval quality relative to other benchmarked BM25 implementations. For this report, we will be comparing with Anserini BM25 based on two metrics: **MRR@10** and **Recall@1000**. Engine for computing MRR@10 and Recall@1000 are Microsoft's `ms_marco_eval.py` & NIST's `trec_eval` respectively.

| Queryset | No of queries | Description |
|---|---|---|
| `queries.dev.small.tsv`| 6980 | Dev queryset used to benchmark Anserini BM25 |

## 5. Rank-safety & effectiveness

### 5.1) Rank Safety

A truly safe Block-Max WAND should, under normal circumstances, return the exact same ranking results as exhaustive BM25. Here, we implemented an exhaustive BM25 variant to compare the two retrieval results. 

| name |    n |   exact_matches |   sum_missing |   sum_extra |
|:-----------------------------|-----:|----------------:|--------------:|------------:|
| random_set.tsv_1000_2000     | 2000 |            2000 |             0 |           0 |
| common_set.tsv_1000_2000     | 2000 |            1999 |             3 |           3 |
| very_common_set.tsv_1000_100 |  100* |              70 |             0 |           0 |
| skewed_set.tsv_1000_100      |  100* |              93 |             1 |           1 |
| rare_set.tsv_1000_2000       | 2000 |            2000 |             0 |           0 |

Here, `sum_missing` and `sum_extra` describes the number of documents returned by exhaustive variant but not BMW variant, and vice versa.

We can see that, for random_set and rare_set, every single document is ranked exactly as in baseline variant. As we includes more terms, the number of query matches starts declining, reaching only $7/10$ hit rate on very_common_set.tsv. Although such hit rate is undesirable, The number of missed documents are actually quite minimal ($3/(1000 * 2000)$ retrieved documents).

This points to ranking-error not being an effect of pruning bugs, but rather, tie-break inconsistencies. This might have happened due to these following reasons.
- Score being computed in 4 byte floating point numbers.
- Differences in traversal order between BMW and exhaustive.

Further research and improvements will aim at fixing these inconsistencies.

(*) : For common and skewed sets, we only evaluate the first 100 queries, since running exhaustive search on all 2000 queries would take substantial amount of time.

### 5.2) Retrieval effectiveness

Compare MRR@10 and Recall@1000 with Anserini BM25. Startorch BM25 is ran with `k1 = 0.82` and `b = 0.68`. Anserini BM25 score is taken from their best tuned attempt for each metric.

| MS MARCO Metrics       | Startorch BMW | Anserini BM25      |
| ---------------------- | ------------- | ------------------ |
| `MRR@10`               | 0.1926        | 0.1892             |
| `Recall@1000`          | 0.8778        | 0.8573             |

Both scores are relatively similar. Slight differernce is potentially due to differences in implementation and tokenization.

## 6. Efficiency: MS MARCO

A Block-Max WAND run on a small 2.9 GB MS MARCO Passage dataset, containing around 8.8 million passages. 

| name                  |    n |   max_rss (MB) |   mean |    p50 |    p95 |    p99 |     max |   min |
|:----------------------|-----:|----------:|-------:|-------:|-------:|-------:|--------:|------:|
| queries.dev.small.tsv | 6980 |   1777.94 |  16.02 | 10.645 | 47.201 | 85.465 | 388.862 | 0.009 |

## 7. Efficiency: OpenAlex

For this experiment, we measure query latency for each query in the 10 described OpenAlex synthetic queryset. We will load all 2000 queries into the engine in a single process, measuring latency per query and engine startup + peak Resident Set Size for that process.

### 7.1) Startup and memory

#### Startup procedure

On engine startup, we will `block_meta.bin` (containing information for each BMW block) and `doc_len_list` (doc_id - doc_len mapping for BM25 length normalization).

- Engine startup latency: $96.373 \pm 2.996$
- Index size on disk: $44.4$ GB
- `block_meta.bin` size on disk: $2.1$ GB
- `doc_len_list` size on disk: $1.6$ GB

Unfortunately, for this experiment, we didn't include a dedicated memory usage measurement for engine startup (though we can estimate it as we'll show in the next section). This will be included in future updates.

#### Max Resident Set Size

For this experiment, all queries from a queryset are loaded and execute sequentially in a single process (including all loaded mmap pages). Hence, data presented below are more representative of "How much of the index the process touched", instead of memory required to run the engine. Worth adding a separate concrete baseline number for future improvements.

| name  |    n |   Max RSS 10 (MB) | Max RSS 1000 (MB) |
|-------------------------------------|------|-----------|-----------|
| random_set.tsv_10                   | 2000 |   11402.7 | 11399.9   |
| common_set.tsv_10                   | 2000 |   12243.1 | 12636     |
| very_common_set.tsv_10              | 2000 |   19754   | 21658.4   |
| rare_set.tsv_10                     | 2000 |   11399.4 | 11401.2   |
| skewed_set.tsv_10                   | 2000 |   19489.1 | 22638.3   |

We can see that RSS scales very clearly with number of postings (spikes for very_common_set & skewed_set), where as it stays at around 11.4 GB for random_set & rare_set. Since posting list for rare_set and random_set are generally very short, 11.4 GB acts as an estimation for baseline memory requirement for engine start-up. 11.4 GB is more than preferable for a 345M documents corpus. Some possible improvements to remedy the memory cost:

- Saving BM25 score per document in posting lists to avoid loading doc_len_list.
- More efficient implementation (avoid RAM intensive data structures like `std::unordered_map`).
- Memory map Block-metadata (Though this does come with query latency cost).

### 7.2) BMW vs Exhaustive

All latency measurements are in **miliseconds (ms)**

#### For k = 10
| name | k | BMW mean | Exh mean | Speedup | BMW p50 | Exh p50 | BMW p99 | Exh p99 |
|:--|--:|--:|--:|--:|--:|--:|--:|--:|
| random_set | 10 | 1.983 | 3.098 | 1.56x | 1.413 | 0.076 | 11.489 | 36.171 |
| common_set | 10 | 5.15 | 48.393 | 9.40x | 2.756 | 5.055 | 54.659 | 827.891 |
| very_common_set | 10 | 1013.88 | 3431.02 | 3.38x | 878.842 | 2102.21 | 3050.8 | 15336.2 |
| rare_set | 10 | 1.238 | 1.567 | 1.27x | 0.921 | 0.756 | 6.578 | 9.917 |
| skewed_set | 10 | 333.689 | 1842.09 | 5.52x | 258.509 | 844.847 | 1485.74 | 8751.83 |

#### For k = 1000

| name | k | BMW mean | Exh mean | Speedup | BMW p50 | Exh p50 | BMW p99 | Exh p99 |
|:--|--:|--:|--:|--:|--:|--:|--:|--:|
| random_set | 1000 | 0.597 | 0.868 | 1.45x | 0.041 | 0.036 | 8.051 | 5.427 |
| common_set | 1000 | 11.29 | 45.98 | 4.07x | 5.644 | 4.447 | 80.076 | 731.608 |
| very_common_set | 1000 | 1291.94 | 3341.37 | 2.59x | 1130.97 | 2065.07 | 3988.49 | 15142.6 |
| rare_set | 1000 | 0.133 | 0.319 | 2.40x | 0.026 | 0.025 | 1.791 | 2.196 |
| skewed_set | 1000 | 504.893 | 1943.1 | 3.85x | 386.489 | 882.63 | 2083.46 | 9224.74 |

*Footnote: Exhaustive benchmark data for very_common_set and skewed_set is computed using only first 100 queries from the full queryset. This is done as computing the full 2000 queries will take took long for a brute force algorithm.*

### 7.3) Query length

| name | k | Short mean | Normal mean | Speedup | Short p50 | Normal p50 | Short p99 | Normal p99 |
|:--|--:|--:|--:|--:|--:|--:|--:|--:|
| random_set | 10 | 0.493 | 1.983 | 4.02x | 0.015 | 1.413 | 3.569 | 11.489 |
| common_set | 10 | 2.021 | 5.15 | 2.55x | 0.736 | 2.756 | 20.658 | 54.659 |
| very_common_set | 10 | 218.796 | 1013.88 | 4.63x | 185.421 | 878.842 | 707.867 | 3050.8 |
| rare_set | 10 | 0.877 | 1.238 | 1.41x | 0.017 | 0.921 | 4.82 | 6.578 |
| skewed_set | 10 | 89.069 | 333.689 | 3.75x | 50.138 | 258.509 | 534.029 | 1485.74 |

*Short set currently lacks k = 1000 runs*

## 8. Analysis

There are a few observations 

### 1) Block-Max WAND latency offset

In queryset random_set k=10, BMW p50 is 1.413 ms and Exhaustive p50 is 0.076, but BMW p99 is 36.2 ms and Exhaustive p99 is 11.5 ms. BMW pays a fixed setup overhead for each query (sorting terms, block metadata bookkeeping) which dominates when posting lists are short. Its pruning effect is only noticeable as posting lists increase in length.

The following two graphs illustrates this offset effect. For a queryset, we sort query latency in ascending order, and then plot it onto a log-scaled latency y-axis.

![Sorted query latency — random_set (k=10): BMW vs. exhaustive](images/random_set_10_latency.png)

![Sorted query latency — common_set (k=1000): BMW vs. exhaustive](images/common_set_1000_latency.png)

Keep in mind that this effect is only noticeable on shorter posting lists. As posting lists becomes longer, traversal cost dominates, leading to BMW beating Exhaustive search in almost every cases.

![Sorted query latency — common_set (k=1000): BMW vs. exhaustive](images/very_common_set_10_latency.png)
*Note: Query latency for BMW for very_common_set is sampled every 20 entries. This is to match Exhaustive's 100 entries while keeping the shape of BMW graph*

### 2) Speedup peaks 

Speedup peaks at common (9.4x) instead of very common (3.4x). This matches Block-Max WAND pruning design. Very common terms has very loose upper_bound to prune against (low IDF, most document likely have saturated scores), Where as less common terms has relatively short posting lists for Block-Max WAND to make a big difference compared to Exhaustive search.

Note that low performance on very_common_set might also be attributed to `domain`, `fields`, and `subfields` duplication issue described in **3. Retrieval Design & Implementation**, which can potentially causes saturation for terms appearing in one of those fields.

### 3) `skewed_set` beats `very_common_set` speedup

For k=10, skewed_set speedup (5.52x) is noticeably better than very_common_set speedup (3.38x). Adding a few terms with high IDF value drives up top-k threshold, allowing more effecting pruning. 

## 9. Threats to validity

Admittedly, there are a few issues with our experimental setup, which we will list in this section.

### 1) Single run config

Querysets are run only once instead of repeating and taking the average. This was done partly due to time and computational constraint of the project. It's also worth mentioning exhaustive runs for very_common_set and skewed_set being limited to 100 queries, which is also due to reasons outlined above.

### 2) Page-cache contamination

A careful reader might notice that, k=1000 variant has significantly lower runtime than k=10 variant. This is due to our experimental setup running k=1000 version of that exact same queryset right after running k=10. This is a well documented issue for our setup, and will be improved in future experiments.

### 3) Lacking RSS benchmarks

The current experiment only measures total RSS of entire process. It doesn't explicitly measures which percentage is from the engine startup, and which is from the actual page caches.

### 4) No field weighting

As explained in **3. Retrieval Design & Implementation**, intuitively, a title match should provide more descriptive value than a keyword or a topic match. We are also well aware of this and is working on a model that better encapsulate this relationship.

### 5) Reproducibility

Current state of project is designed to work on current experimental setup first, which is to say, "it works on my machine". Part of the implementation either only works with specific compilers (\_\_FILE_NAME\_\_)

## 10. Conclusions and future work

In this report, we've outlined out architectural design for our High Recall IR system for OpenAlex Works entities. We've also benchmarked rank-safety, retrieval effectiveness, and performance gain compared to exhaustive search. While rank-safety is still pending for a resolution, both retrieval effectiveness and performance gain is promising compared to our original full search proposal at 345M documents scale.

Future work will aim on improving rank-safety of our Block-Max WAND, as well as experimenting with methods to further reduce memory usage, without trading too much performance. We will also work on improving our experiment robustness in future works.

Along with that, we will starts incoperating our PageRank implementation for Startorch (which is long overdue at this point). as well as connecting these two components for a complete retrieval system.

## References

- **WAND (Weak AND)** — Broder, Carmel, Herscovici, Soffer, Zien, [*"Efficient Query Evaluation using a Two-Level Retrieval Process"*](https://www.researchgate.net/publication/221613425_Efficient_query_evaluation_using_a_two-level_retrieval_process)
  (CIKM 2003).

- **Block-Max WAND (BMW)** — Ding & Suel, [*"Faster Top-k Document Retrieval Using Block-Max Indexes"*](https://research.engineering.nyu.edu/~suel/papers/bmw.pdf)
  (SIGIR 2011). 

- [PISA](https://github.com/pisa-engine/pisa) 

- Manning, Raghavan & Schütze, *Introduction to Information Retrieval*