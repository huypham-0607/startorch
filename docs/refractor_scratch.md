# Refractor scratch

## Expose python binding to a single query class

### 3.1 Optimize build I/O before rerunning

**At full-en scale** (about 13.7B tokens, 9B partial-block postings, 346M documents, 83M blocks):

- **Token-stream reads are CPU-bound, not disk-bound.**
  - Each pass spends about 17 minutes in stdio calls even with a warm cache, against about 2 minutes chunked. The build makes two passes.
  - The drive could deliver the whole stream in under a minute.
- **The observed doc-length pass was slower still:** about 31 minutes, roughly 120 MB/s. Two things likely made it worse:
  - glibc sizes each `FILE` buffer from the filesystem block size, which is 4 KB here, so every refill is a 4 KB `read()`: about 55M syscalls per pass.
  - The tokenizer's DuckDB connection, including its `_token_stream` temp table, stays open until `PostingBuildler.build()` returns, so the C++ steps run beside it. RSS was 20 GB, with 18 GB of swap in use, during tokenizing. It was not measured during the doc-length step.
- **Merge reads** cost about 10 minutes, against about 30 s chunked.
- **Engine load** spends about 24 s on doc lengths and about 30 s on block metadata, out of its 96 s.
- **The C++ writers are not a bottleneck** at 8 ns per posting.
- **The Python token writer is a bottleneck.** After the first token file appeared, the remaining 204 took 104 minutes, about 30 s per 1.1 GB file or 450 ns per token. That time is spent in `fetchmany` plus the per-token `struct.pack` loop.

**Root causes.**

- Every C++ reader goes through `FILE*` with a 4 KB buffer and several locked stdio calls per record:
  - `read_token` does three `fread`s and an `ftell`;
  - `read_vbe` does one `fread` per byte;
  - `SafeFile::fread` calls `ftell` before every read.
- The tokenizer's DuckDB memory is still held while the C++ steps run.
- The Python token and lookup writers pack one record at a time.

**(IMPORTANT) Plan.**

1. **Add a `BufferedReader` to `file_io`,** the counterpart of `BufferedWriter`:
   - one `read()` per refill into a large owned buffer;
   - typed reads and VBE decoding straight from the buffer;
   - clean-vs-truncated end-of-file detection without `ftell`; - Ensure backward compatibility.
   - `posix_fadvise(POSIX_FADV_SEQUENTIAL)`.

   The buffer size is a parameter: large (e.g. 64 MB) for the sequential token passes, small (e.g. 4 MB) per stream in merge, which opens every partial block at once (234 on full-en).
2. **Port every reader to it:** `read_token`, merge's `Stream`, `read_doc_len_list`, `read_block_meta_file`, and `read_metadata`. Drop the per-read `ftell` in `SafeFile::fread`.
3. **Writers:**
   - raise `BufferedWriter`'s buffer from 1 MB;
   - copy `size * count` bytes with one `memcpy` instead of one per element;
   - write through the file descriptor instead of a second `FILE` buffer.
    -    This is lower priority, since writes measured fast.
4. Consider having size a fixed constant, and implement buffer as a fixed size array on stack.
    - Instead of buf_size as param, hard code + document as a fixed size (e.g. 64 MB)
    - Implement buffer as a fixed size array on stack to speed up IO procecss.
    - Consider corner cases (end of file flags) & handling write requests larger than 64 MB.
    - Consider tradeoff for fixing buffer size, and if its worth it for a on-stack buffer.


4. **Python:**
   - close the tokenizer's DuckDB connection before `build()` starts the C++ steps;
   - write token and lookup files from numpy batches (the tokenizer's Python half, §2), which replaces the per-token writer.
5. **Related, already in §4:** counting doc lengths during the SPIMI pass removes one of the two token-stream passes entirely. Since full-en must be rebuilt anyway, decide whether it lands before this rebuild.
- Potentially process with this, but layout short summary of how you are planning to compute doc-len right in SPIMI before executing.

Profile both once the readers are fixed.

**Verification.**
- **C++ tests** all pass, plus new reader tests: records split across buffer refills, a VBE value split across a refill, and clean vs truncated end of file.
- **`io_bench`** shows the chunked numbers for the ported readers. It is in the session scratchpad now and worth moving to `cpp/benchmarks/`.
- **The math-en end-to-end comparison** (§3.2) is byte-identical with the new readers and writers, with each build step timed before and after.


## Non-IO related optimizations

Not I/O, but in the same per-record loops:
- merge's `Stream::get_term()` returns a string copy, compared once per posting;
- SPIMI looks each token's term up in the hash map three times.

## Redundant Metadata absolute paths.
Metadata still stores absolute paths. load_index now ignores them, so renamed indexes load, but the fields and the write-only .txt twin are dead weight. Fix: store only parameters and a format version, in the next format change.

## **Metadata still stores absolute paths.**
`load_index` now ignores them, so renamed indexes load, but the fields and the write-only `.txt` twin are dead weight. Fix: store only parameters and a format version, in the next format change.
## **Two ways to build an index.**
Python uses only the bindings, while the CLI apps are the only place k1 and b get validated. BM25 defaults live in four places, and one stage has four names, including a typo in the exported `PostingBuildler` class. Fix: move validation into the library function, keep the app. Validate that the validation function works since there might be discrepancies between config used for Anserini comparison vs the actual bounds in the app validation.
## **Two full passes over the 210 GB token stream.**
Doc lengths and SPIMI construction each read everything, and merge silently depends on a file a separate call wrote into the same folder. Fix: one build entry point that counts doc lengths during the SPIMI pass and enforces the order itself.
## **Smaller items.**
VBE decoding is reimplemented for mmap because the VBE utilities only accept a `FILE*`. A buffer-size constant is defined in three files, and the logger's two overloads duplicate each other.

## IMPORTANT NOTE

Before executing the refractor, validate the feasibility of every single action item, outline a short, 2,3 bullet points summary on executing plan for each action item. DO NOT EXECUTE yet.

After executing the plan, update refractor.md to reflect the update.

