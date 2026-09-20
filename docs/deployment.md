# Deploying Startorch as a live search service (EC2 + S3)

A staged plan from "a local CLI that queries an index on `/data`" to "a URL that answers search requests".
Each step gives the goal, a high-level outline, and terms to research. No code here on purpose: the point is
to know what to build and what to read before building it.

Written against the project state on 2026-09-19.

---

## 0. Where the project stands today

**What already works**

- `Searcher` loads one index and answers queries in-process (`python/src/startorch/search.py`).
- `QueryEngine` (C++, pybind11) does Block-Max WAND top-k BM25 and returns `(score, mapped_id)`.
- `DocIdLookup` maps those back to OpenAlex ids.
- `boto3` is already a dependency, so S3 access needs no new packages.

**What serving needs that does not exist yet**

| Gap | Why it matters for a server |
|---|---|
| No HTTP layer | Nothing accepts a request. `cli.py` prints to stdout. |
| The bindings never release the GIL | Two requests cannot run queries at the same time in one process. See step 2. |
| Results carry ids and scores only | A search page needs titles, authors and years. The index stores none of them. See step 3. |
| Query tokenization runs through DuckDB | The server process must carry DuckDB plus its `fts` extension, and pay that cost per query. |
| No limits anywhere | `k`, query length and request rate are all unbounded. A public endpoint will be abused. |

**Numbers that drive every sizing decision** (measured 2026-09-19, full-en)

| Quantity | Value |
|---|---|
| Files needed to serve | `posting_*.bin` 47.7 GB, `block_meta.bin` 2.25 GB, `doc_len_list.bin` 1.73 GB, `doc_id_lookup.bin` 4.15 GB, metadata — **about 56 GB** |
| Files *not* needed to serve | `token_stream/` (~220 GB), `posting/partial/` — build inputs only |
| Heap after load | 6.7 GiB |
| Heap peak during load | 8.7 GiB |
| Load time | 14 s with a warm page cache; budget more from cold storage |
| Posting pages touched per 200 queries | 0.25 GiB (rare terms) to 12.7 GiB (very common) |

So: one instance, at least 16 GiB of RAM, about 70 GB of fast disk, and a slow start. That shape decides
most of what follows.

---

## Phase A — make the engine serveable (all local, before any AWS spend)

### Step 1. Put an HTTP layer around `Searcher`

**Goal.** `GET /search?q=...&k=10` returns JSON, with the index loaded exactly once at startup.

**Outline.**
- One process holds one `Searcher` created at startup, not per request. Loading costs 14 s and 6.7 GiB.
- Endpoints worth having from the start: `/search`, `/healthz` (process alive), `/readyz` (index loaded).
  The split matters later, because the load balancer must not send traffic during those first seconds.
- Return the search time your engine already measures, alongside the results, so latency is visible from
  outside without a profiler.
- Cap `k` server-side and reject over-long queries. Treat any client-supplied number as hostile.

**Research.** FastAPI, ASGI vs WSGI, uvicorn, lifespan/startup events, Pydantic request validation,
health check vs readiness check, HTTP status codes for rejected input (400 vs 422).

### Step 2. Decide how concurrency works — the most important design decision here

**Why.** `cpp/python/bindings.cpp` binds `QueryEngine::query` with no `py::call_guard<py::gil_scoped_release>`.
The GIL is therefore held for the whole search. Threads in the server will queue behind each other, and a
single 1.2 s query on very common terms blocks everything.

**Three ways out, with their costs.**

1. **Release the GIL in the binding.** Cheapest change, but only correct if `QueryEngine::query` is safe to
   run from several threads at once. Review that first: the query methods are not `const`, and they share the
   memory-mapped files and the doc-length vector. Read-only sharing is fine; any mutation of engine state is
   not.
2. **Several worker processes.** Standard for Python servers, but each process would load its own 6.7 GiB.
   Two workers will not fit in 16 GiB. If you go this way, preload the index in a parent and `fork`, so the
   heap is shared copy-on-write and the mapped posting pages are shared by the page cache anyway.
3. **One worker, an explicit queue, and honest timeouts.** Simplest and predictable. Throughput is then one
   query at a time, which may be entirely acceptable for a portfolio service.

Decide this before sizing an instance, because options 1 and 2 change how many cores and how much RAM are
worth paying for.

**Research.** Python GIL, `py::call_guard<py::gil_scoped_release>`, data races vs benign read sharing,
`const`-correctness and thread safety, gunicorn `preload_app`, copy-on-write after `fork`, uvicorn workers,
queueing theory basics (utilization vs latency, why a queue explodes near full utilization).

### Step 3. Decide what a result actually shows

**Goal.** Turn `(id, score)` into something a person can read.

**Outline.**
- The index deliberately holds no titles. Something else must map doc id to title, authors, year, DOI.
- Options, cheapest first: a Parquet or DuckDB file of `(id, title, year, ...)` on the same instance, read at
  query time for the top k only; SQLite with an index on id; a key-value service if you later split serving
  across machines.
- Size it before choosing: a few hundred million rows of title text is not small. You may decide to serve
  metadata only for the corpus you actually expose.
- This is also where the xpac/deleted-works problem from `CLAUDE.md` becomes visible: if you keep those
  records in the index, they will appear in results with no working OpenAlex link.

**Research.** Forward index vs inverted index, document store, DuckDB point lookups over Parquet, SQLite
`mmap_size`, DynamoDB single-table design, key-value store latency budgets.

### Step 4. Make the build reproducible off your machine

**Goal.** The same binary and environment on the server as locally, without hand-installing anything.

**Outline.**
- Your `.so` is built locally against Arch's toolchain and glibc. The server will run Amazon Linux or Ubuntu.
  Build for the target, not on your laptop.
- A container is the usual answer: multi-stage build, compile the C++ in a builder stage, copy the `.so` and
  the Python package into a slim runtime image. It also documents the runtime dependencies (DuckDB and its
  `fts` extension) whether or not you deploy containers.
- Keep the index out of the image. It is 56 GB and changes on a different schedule than the code.
- If you consider Graviton (arm64) for price, note the index files are written in native byte order and
  native struct sizes. x86_64 and arm64 are both little-endian with the same widths here, but verify by
  building a small index on both and comparing, rather than assuming.

**Research.** Docker multi-stage builds, glibc version compatibility, manylinux, Amazon Linux 2023, ECR,
AWS Graviton, endianness and struct padding in binary formats, `uv` in containers.

---

## Phase B — index artifacts in S3

### Step 5. Design a versioned bucket layout

**Goal.** An index build is an immutable artifact you can point servers at, roll forward and roll back.

**Outline.**
- One prefix per build, for example `indexes/full-en/2026-09-19/`, holding exactly the serving files from
  the table above. Never overwrite a live prefix in place.
- Store a small manifest next to each build: profile, build parameters, document count, checksums, source
  snapshot date. Your `metadata.txt` is a good start; the snapshot date matters given the corpus drift
  already recorded in `CLAUDE.md`.
- Upload with a tool that does multipart and checksums. Verify after upload; a silently truncated posting
  file fails in ugly ways at query time.

**Research.** S3 prefixes and "folders", multipart upload, `aws s3 sync`, S3 object versioning, checksum
algorithms (`--checksum-algorithm`), S3 Standard vs Standard-IA, lifecycle rules, immutable artifacts,
blue/green deployment.

### Step 6. Get the index onto the instance — and know what not to do

**Goal.** The engine reads local files fast, and a new instance becomes ready without you copying by hand.

**Outline.**
- The engine memory-maps posting files and depends on random reads. **Do not try to serve directly from S3.**
  Mountpoint for Amazon S3 and s3fs exist, and both turn every page fault into a network round trip.
- Sane options:
  - **Download at boot** from S3 to a local volume. Simple, and the first boot is slow (56 GB).
  - **EBS snapshot of a prepared index volume.** Attach a volume created from the snapshot at launch. Faster
    to a ready state, but blocks are fetched lazily on first touch unless you warm them or pay for Fast
    Snapshot Restore.
  - **Instance store NVMe** (i-family). Fastest and cheapest per GB, but the data is gone when the instance
    stops, so it must be repopulated from S3 at every boot.
- Whichever you choose, measure the first query after boot separately from steady state. A cold page cache
  is the difference between 14 s and minutes.

**Research.** Mountpoint for Amazon S3 (and why random access is wrong here), EBS gp3 (baseline 3000 IOPS /
125 MiB/s, provisioned up to higher), EBS snapshots, Fast Snapshot Restore, instance store volumes, EC2 user
data, cloud-init, page cache warming (`vmtouch`, `fio`), VPC gateway endpoint for S3 (avoids NAT charges).

---

## Phase C — run it on EC2

### Step 7. Rehearse on a small index first

**Goal.** Prove the whole path — build, upload, boot, load, serve, monitor — for a few cents.

**Outline.**
- Use msmarco (0.3 GiB of heap) or math-en. Everything except sizing behaves the same.
- Get a request answered end to end over the public internet before touching full-en.
- Only then size the real instance.

**Research.** EC2 instance types t3/t4g, AWS Free Tier limits, SSM Session Manager (shell access without SSH
keys or open port 22), instance profiles.

### Step 8. Size and launch the full-en instance

**Goal.** An instance that holds the working set without swapping.

**Outline.**
- RAM: 6.7 GiB of heap plus room for posting pages. 16 GiB works but leaves common-term queries reading from
  disk; 32 GiB is comfortable. Memory-optimized (r-family) is the natural family.
- Disk: about 70 GB for the index, plus the OS. Throughput matters more than capacity for load time.
- Turn swap off, or keep the service out of swap. Swapped heap is far worse than evicted posting pages: the
  doc lengths and block metadata are touched on every query.
- Run the service under a process supervisor so it restarts on failure and starts on boot.
- Keep the instance in a private subnet with no public IP. Reach it through Session Manager.

**Research.** EC2 r7i/r7g families, on-demand vs Spot vs Savings Plans, `vm.swappiness`, systemd unit files
and `Restart=`, `MemoryMax` and `MemorySwapMax` in systemd, VPC public vs private subnets, security groups
vs NACLs, IAM instance profile, least privilege for the S3 read path.

---

## Phase D — make it a public service

### Step 9. Front door: load balancer, TLS, domain

**Goal.** `https://search.yourdomain.com` reaches the instance safely.

**Outline.**
- Put an Application Load Balancer in front, terminating TLS with a free ACM certificate. The instance itself
  never faces the internet and accepts traffic only from the balancer's security group.
- Point a Route 53 record at the balancer.
- Configure the health check against `/readyz`, with a grace period longer than the index load, and make
  deregistration wait for in-flight queries.

**Research.** Application Load Balancer, target groups, health check grace period, deregistration delay, AWS
Certificate Manager, Route 53 alias records, HTTP to HTTPS redirect, security group chaining.

### Step 10. Protect it from load and from cost

**Goal.** One person with a script cannot exhaust the machine or your budget.

**Outline.**
- Rate-limit per IP at the edge. A single very common query already costs about a second of CPU.
- Enforce server-side caps: maximum `k`, maximum query length, request timeout, and a bound on concurrent
  queries.
- Cache. Real query traffic is heavily repeated, and an in-process LRU of `(query, k)` results is a few lines
  with a large effect. A CDN in front can cache identical URLs outright.
- Set a budget alarm on day one, before the first full-size instance runs overnight.

**Research.** AWS WAF rate-based rules, CloudFront caching and cache keys, `Cache-Control`, LRU caches,
bulkhead and timeout patterns, AWS Budgets and Cost Anomaly Detection, AWS Pricing Calculator.

### Step 11. See what it is doing

**Goal.** Answer "is it up, how fast is it, and why did it get slow" without logging in.

**Outline.**
- Ship application logs and metrics: queries per second, latency percentiles (p50/p95/p99, not averages),
  error rate, cache hit rate, index load time at startup.
- EC2 does not report memory usage by default. Install the CloudWatch agent if you want to see the very thing
  that constrains this service.
- Alarm on the few things that mean real trouble: readiness failing, swap in use, p99 latency, 5xx rate.

**Research.** CloudWatch agent, custom metrics, embedded metric format, log groups and retention, percentile
statistics, alarms and composite alarms, structured logging.

---

## Phase E — keep it running

### Step 12. Refresh the index without downtime

**Goal.** Ship a new corpus build without a maintenance window.

**Outline.**
- Build offline (locally, or on a Spot instance), upload to a new S3 prefix, bring up a second instance on the
  new prefix, wait for readiness, then switch the target group and retire the old one.
- Keep the previous build in S3 until the new one has served real traffic.
- The corpus itself only changes quarterly (see `CLAUDE.md`), so this runs rarely. That is an argument for
  keeping it manual and documented rather than automated early.

**Research.** Blue/green deployment, immutable infrastructure, EC2 Spot interruptions and checkpointing,
launch templates, AMI baking with Packer or EC2 Image Builder.

### Step 13. Write the infrastructure down

**Goal.** Rebuild the whole environment from a file, not from memory of console clicks.

**Outline.**
- Click through the console once to learn the pieces, then capture the result as code.
- Store non-secret settings (bucket, prefix, profile name) outside the image so the same artifact runs in
  staging and production.

**Research.** Terraform or AWS CDK or CloudFormation, SSM Parameter Store, twelve-factor configuration,
tagging strategy for cost allocation.

---

## Anti-patterns to avoid

- **Memory-mapping posting files straight from S3.** Random access over the network; the pruning algorithm
  makes it worse, not better.
- **Autoscaling this service on CPU.** A new instance needs tens of seconds and 56 GB before it can answer
  anything. Scale by adding pre-warmed instances deliberately, not reactively.
- **Several worker processes each loading their own copy of the index.** Check the arithmetic against RAM
  first.
- **A public IP on the instance with the service port open.** Load balancer in front, instance private.
- **Shipping `token_stream/` or `posting/partial/` to the server.** They are build inputs, several times the
  size of what serving needs.

## Decisions to make before writing code

1. Concurrency model (step 2) — this one constrains instance size, cost and the API's behavior under load.
2. Whether results show metadata, and where that metadata lives (step 3).
3. Whether xpac and deleted works stay in the served index (see `CLAUDE.md`).
4. Storage mode: download at boot, EBS snapshot, or instance store (step 6).
5. Budget ceiling, and whether the service runs 24/7 or only when you are showing it.
