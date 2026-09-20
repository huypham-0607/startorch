# Retrieval Engine for Startorch

## 1. Okapi BM25 lexical retrieval

BM25 is a lexical scoring function. It quantify document relatedness to a certain query according to these metrics:

- No of documents containing a word $x$.
- No of occurences of word $x$ in a document $y$.
- Length of certain document $y$ (and by extension, average document length for normalization).

Given a query $Q$, containing $n$ keyword $q_{1}, q_{2}, \ldots q_{n}$, the BM25 score for document $D$ is:

$ \large score(D,Q) = \sum_{i=1}^{n}IDF(q_{i}) \cdot \frac{f(q_{i},D) \cdot (k+1)}
{f(q_{i},D) + k \cdot (1-b+b\cdot \frac{|D|}{avgdl})}$

Where:
- $f(q_{i},D)$: Frequency (occurences) of $q_{i}$ in document $D$
- $|D|$: Length of document $D$
- $avgdl$: Average length of all documents.
- $k$,$b$: Adjustable parameters.

Here, $IDF(q_{i})$ is the **Inverse Document Frequency** of a keyword, defined as:

$IDF(q_{i}) = ln(\frac{N-n(q_{i})+0.5}{n(q_{i})+0.5}+1)$

Loosely speaking, it quantifies "**rarity**" of a keyword among documents. The more documents containing $q_{i}$, the lower $IDF(q_{i})$ is.

The term $ \frac{f(q_{i},D)\cdot(k+1)}{f(q_{i},D)z+k(\ldots)} $ represents the information gain of $q_{i}$ appearing $f(q_{i},D)$, up to an upper limit of $(k+1)$.

Parameter $k$ adjusts both the **upper bound** and the **convergence rate** of the function. $k$ is generally in range of $[1.0,2.0]$

The term $1-b+b\frac{|D|}{avgdl}$ represents the **length normalization** for a document. The higher the document length, the more dilluted the score for each keyword will be.

Parameter $b$ adjust the dilluting effect of the document length.

Generally, the best value for $k$ and $b$ are $b \in [0.5,0.8]$ and $k \in [1.2,2.0]$

*Note: These concepts are very loosely explained. I will write a separate write-up on derivation of BM25 from the binary independence model, and its interpretation in detail.*

## 2. Implementation details

### 2.1) Key issues to consider

A few issues:
- Consider weight of Domain/Field/Subfield when two or more topics shares the same LCA
- Consider weight of certain word $x$ when it appears in multiple columns (Title/topics and its hierarchy/keywords).
- Consider utilizing weight of **topic/keyword scores** provided by OpenAlex.
- Consider the usage of **abstract** and its issue (High null rate, dilluted keyword/topic score due to length normalization)

### 2.2) Design

We will use Block-Max WAND with BM25 as our core ranking metric.

#### 2.2.1) Tokenization

We will construct our document by concatenating elements across 6 fields

| Field name                | Type          | Desc                                                              |
| ------------------------- | ------------- | ------------------------------------------------------------------|
| `title`                   | `VARCHAR`     | Title of given work |
| `topics`                  | `VARCHAR`     | OpenAlex assigned topics of given work |
| `subfields`               | `VARCHAR`     | OpenAlex assigned subfields of given work |
| `fields`                  | `VARCHAR`     | OpenAlex assigned fields of given work |
| `domains`                 | `VARCHAR`     | OpenAlex assigned domains of given work |
| `keywords`                | `VARCHAR`     | OpenAlex assigned keywords of given work |


As a preliminary design, we will consider each single word as a token. Hyphen connected words are collapsed an considered a single word. For instance "We value your well-being" will be considered 5 tokens: "We", "value", "your", "well", "being". For the scope of this project, machine learning based tokenization methods are not considered.

Normalization rules are as follows:
- All accents are stripped.
- All tokens will be decapitalized.
- All non-alphabetical characters are stripped (This is of minimal impact for our dataset, since our fields are generally low-nuance).
- Stopwords are stripped from the concatenated document text *before* tokenization (a fixed English stopword
  list, matched with regex word boundaries, replaced globally — not just the first occurrence).

For stemmer, we use DuckDB FTS's built-in `stem(token, 'english')` function directly in SQL — this is
Snowball/Porter2 under the hood, not the original 1980 Porter algorithm (`'porter'` is a separate, worse
option in the same extension). No Python stemming library needed. Known over-stemming collisions (e.g.
`organization`/`organic`/`organ` all reducing to `organ`) were tested and found to occur under both variants —
inherent to rule-based stemming generally, not fixable by switching stemmer choice. Accepted as sufficient for
BM25: IDF naturally discounts collision-heavy stems, and multi-term queries dilute single-term noise.

#### 2.2.2) Inverted Index List (Posting List) — implemented

**Language boundary**: Python's job stops at producing tokens (2.2.1), plus one more thing — a dense doc_id
remapping (`tokenizer.py`'s `build_doc_id_lookup`, writing `doc_id_lookup.bin`). Raw OpenAlex numeric IDs are
sparse and can exceed `int32`, so every document gets ranked into a dense `[0,N)` id once, up front; everything
downstream (posting lists, doc lengths, and eventually the graph) indexes by this id instead, which is what
makes flat-array O(1) lookups possible everywhere else. Everything past tokenization — dictionary, inversion,
sorting, block-max metadata, serialization, merge — is C++.

**Build pipeline, three stages, each its own file** (`cpp/src/lexical/`):

| Stage | File | Output |
|---|---|---|
| SPIMI partial-block construction | `construct_inverted_blocks.cpp` | `block_*.bin`, one set per memory-limited chunk |
| Document length table | `construct_doc_len_list.cpp` | `doc_len_list.bin`, dense array by doc_id |
| K-way merge + BMW block-metadata | `merge_inverted_blocks.cpp` | `posting_*.bin` + one consolidated `block_meta.bin` |

Shared pieces both later stages depend on: `posting_list.cpp` (`PostingItem`/`PostingList`), `bm25.cpp`
(`calc_BM25`/`bm25_saturation`), `token_stream.cpp` (`read_token`, the tokenizer wire-format reader).

**Per-term storage — two arrays, not one flat list**, as planned, now with the open questions resolved:

| Array | Contents | Purpose |
|---|---|---|
| Block metadata (`BlockMeta`, in `block_meta.bin`) | `doc_id` (block's start doc), `start_addr`, `block_ub` | Scanned to decide skip/no-skip *without* touching postings |
| Posting data (`posting_*.bin`) | `doc_id` delta + `tf`, sorted, delta-encoding resets at each block boundary | Only read for blocks that survive pruning |

- **`file_index` lives on `TermMeta`, not `BlockMeta`**: `build_posting_list` writes one term's entire posting
  list in a single call, so every block it produces for that term always lands in whichever `posting_*.bin`
  file was open at the time — a term's blocks never span multiple files, so storing `file_index` per-block was
  pure redundancy (one term can have many blocks, each paying for the same repeated 4 bytes).
- **Block size**: a fixed posting count per block (constructor parameter, default 128), not a byte budget.
- **Score vs. impact, resolved**: `block_ub`/`term_ub` store the *full* BM25 upper bound (`IDF * saturation`),
  not raw impact. This ties the index to whatever `k1`/`b` it was built with — retuning needs a full rebuild —
  accepted as the tradeoff. Computed in two passes since `df_t` (needed for `IDF`) isn't known until a term's
  postings are fully merged: the saturation term is computed per-block during the merge, then every block's
  bound is multiplied by `IDF` retroactively once `df_t` is final.
- **`BlockMeta.doc_id` is always absolute in memory**, binary-searchable directly with no reconstruction step
  by the caller. Only the on-disk *serialization* delta-encodes consecutive blocks' start `doc_id` (VBE
  compressed, same scheme as posting deltas) — `write_block_meta_file` encodes the delta, `read_block_meta_file`
  reconstructs the absolute value while loading.
- **One consolidated `block_meta.bin`**, not one file per term and not scattered across the `posting_*.bin`
  files. It's small relative to the posting data it describes (roughly `block_size`:1 smaller), so it's meant
  to be fully loaded/`mmap()`-ed and kept resident for the query engine's whole lifetime, while posting data
  stays properly out-of-core.
- 89 GoogleTest cases across the files above (`ctest --test-dir build`, or `./build/<name>_tests` per file).

See §2.2.3 below for query-time traversal, the part that actually answers a query using this index.

#### 2.2.3) Query-time traversal — implemented

This is the part that turns a list of search terms into a ranked list of documents, without scoring every
document that contains any of the terms.

**How it works, briefly**: each search term gets its own cursor (`PostingPointer`) into that term's posting
list, starting at the term's first document. On every round, the engine sorts the cursors by the document ID
each one currently points at, and figures out how far it would need to read before a document could possibly
beat the current top-k score threshold — this is the "pivot" document. Two shortcuts keep this cheap:

- **Term-level shortcut**: if even the best possible score across every remaining term can't beat the current
  threshold, skip past all of them at once — no document gets scored.
- **Block-level shortcut**: once a cursor is moved forward to a block, if that block's own maximum possible
  score can't help, skip the whole block without decoding a single posting inside it.

Only documents that survive both shortcuts ever get a real BM25 score computed. This is the dynamic-pruning
behavior Block-Max WAND is named for (see the papers in §3 below) — for a typical query it scores a small
fraction of the documents that actually contain the search terms, instead of all of them.

**Status**: implemented and tested — `cpp/src/lexical/query_engine.cpp`, 45 dedicated tests on top of the 89
from the rest of the C++ retrieval pipeline (134 total, `ctest --test-dir build`). Two real bugs were caught by
these tests, not by manual review: one function computed the correct "skip to here next" position and then
returned a placeholder value by mistake instead of using it; a second function read metadata from the wrong
path and silently returned zero results for every query, regardless of what was actually indexed. Both are
fixed, and both now have a regression test guarding them.

**Not yet done**: there is no way to run a query from outside C++ yet. The plan is for Python to turn a query
string into search terms — reusing the same tokenizer used to build the index, so terms match exactly — and
call into the C++ engine to get results back, plus a real CLI command for it. That hookup is the current next
step; see the project root `README.md` for the up-to-date roadmap.

## 3. Readings

### 3.1) Core algorithms, roughly in reading order

- **MaxScore** — Turtle & Flood, [*"Query Evaluation: Strategies and Optimizations"*](https://research.engineering.nyu.edu/~suel/papers/bmm.pdf) (1995). The original term-at-a-time pruning
  idea: BM25's saturation bounds any single term's max contribution, so documents that can't possibly beat the
  current top-k threshold get skipped.
- **WAND (Weak AND)** — Broder, Carmel, Herscovici, Soffer, Zien, [*"Efficient Query Evaluation using a
  Two-Level Retrieval Process"*](https://www.researchgate.net/publication/221613425_Efficient_query_evaluation_using_a_two-level_retrieval_process)
  (CIKM 2003). The standard starting point — reduces full scoring by >90% with near-zero recall loss.
  Maintains posting-list iterators sorted by doc ID, skips using per-list max-score bounds.
- **Block-Max WAND (BMW)** — Ding & Suel, [*"Faster Top-k Document Retrieval Using Block-Max Indexes"*](https://research.engineering.nyu.edu/~suel/papers/bmw.pdf)
  (SIGIR 2011). Modern default — stores score upper bounds per *block* of postings instead of per whole list,
  enabling much tighter skipping. What production systems (Lucene 8+, etc.) actually run today.
- Follow-ups worth knowing exist, not essential first reads: Dimopoulos, Nepomnyachiy & Suel,
  [*"Optimizing Top-k Document Retrieval Strategies for Block-Max Indexes"*](https://research.engineering.nyu.edu/~suel/papers/bmm.pdf)
  (WSDM 2013); Mallia & Porciani, [*"Faster BlockMax WAND with Longer Skipping"*](https://www.antoniomallia.it/uploads/ECIR19a.pdf)
  (ECIR 2019) — variable-block refinements.

### 3.2) Background / grounding

- Manning, Raghavan & Schütze, *Introduction to Information Retrieval*, Ch. 7 ("Computing scores in a complete
  search system") — free at [nlp.stanford.edu/IR-book](https://nlp.stanford.edu/IR-book/). Covers static
  pruning (champion lists, tiered indexes) and dynamic pruning conceptually before WAND's specific mechanics —
  good to read before the papers above.

### 3.3) Reference implementations

- [PISA](https://github.com/pisa-engine/pisa) — production-grade C++ IR research engine implementing MaxScore,
  WAND, Block-Max WAND, Variable Block-Max WAND. Real code to study; a full system, not a minimal example.
- [ajikan/WAND-Implementation](https://github.com/ajikan/WAND-Implementation) — small, single-purpose C++ WAND
  implementation. Better first read than PISA for seeing the core algorithm without the surrounding engine.
- [Vespa: WAND — Accelerated OR search](https://docs.vespa.ai/en/ranking/wand.html) — practical explanation
  from a real production system's perspective, good alongside the papers.