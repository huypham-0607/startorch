# Deploying Startorch as a live search service (EC2 + S3)

A plan to put the query engine behind a URL, sized for **a small number of users** — a portfolio demo, a few
friends, an occasional reviewer. Not a product launch.

Two rules shaped it: **write as little as possible**, and **depend on as little as possible**. Those pull
against each other, so where a managed service costs real money or real setup time, this picks the boring
option that ships.

Last updated 2026-09-21. The current status is in the next section. Check current docs before pinning versions.

---

## Status

**The HTTP API works and is tested locally. Nothing is deployed yet.**

| Step | Status | Notes |
|---|---|---|
| 1. FastAPI wrapper | Done | `/search`, `/healthz`, `/readyz`. No result cache yet. |
| 2. Concurrency | Done | The GIL is released. The engine and tokenizer are thread-safe. 4 searches run at a time. |
| 3. Results without a metadata store | Done | The API returns OpenAlex ids with the `W` prefix. The client fetches titles. |
| 4. Docker image | Not started | |
| 5. Index in S3 | Not started | The full-en and msmarco indexes are built in the current format, ready to upload. |
| 6. Compose + Caddy | Not started | |
| 7. Protection | Partly done | Input limits and the search limit are in. No request timeout yet. |
| 8. Operations | Not started | The health endpoints are ready for an uptime monitor. |

### What works now

- **Endpoints.** `/search?query=&k=` returns `{query, k, took_ms, hits: [{id, score}]}`. `k` must be 1 to 100, and
  the query 1 to 512 characters. Anything else returns 422.
- **Background loading.** The server starts at once and loads the index on a worker thread. `/healthz` answers
  during the load. `/readyz` and `/search` return 503 until the load finishes.
- **Failed load.** Both health checks return 503, because only a restart can fix it.
- **Searches off the event loop.** Each search runs on a worker thread, at most 4 at a time. A slow search does not
  block other requests, including `/healthz`.
- **Thread safety.**
  - In C++, the query path is `const`, so the compiler rejects any write to shared state.
  - ThreadSanitizer finds no data race when 8 threads share one engine.
  - The tokenizer gives each thread its own DuckDB cursor.
- **Tests.** `uv run pytest` runs 85 Python tests in about 17 s, including the API. `ctest` runs 198 C++ tests.
- **Run it locally.** From `python/`, run `fastapi dev src/startorch/api/api.py`. The profile is the `PROFILE`
  constant in `api.py`, `msmarco` by default.

### Measurements

| Quantity | Value |
|---|---|
| Files needed to serve full-en | About 56 GB: posting files 47.7 GB, block metadata 2.25 GB, document lengths 1.73 GB, id lookup 4.15 GB |
| Files not needed to serve | `token_stream/` (about 220 GB) and `posting/partial/`. They are build inputs only. |
| Memory after load, full-en | 6.7 GiB |
| Memory peak while loading, full-en | 8.7 GiB |
| Load time, full-en | 14 s with a warm cache; longer from a cold disk |
| Load time and memory, msmarco | 0.4 s, 0.3 GiB |
| Parallel search speedup | 1.7× on 2 threads, 2.7× on 4, 3.6× on 8 |
| Longest event-loop pause during a load | 0.006 s (0.41 s before the GIL was released) |

So the target is one instance with 16 GiB of RAM and about 70 GB of disk.

### Next

1. Write the Dockerfile and `compose.yaml` (steps 4 and 6). Test them locally with the msmarco profile.
2. Make the profile an environment variable instead of a code constant, so one image can serve any index.
3. Add a request timeout and the result cache (steps 1 and 7).
4. Upload the full-en index to S3, and run it on EC2 (steps 5 and 6).
5. Set up the uptime monitor and the budget alarm (step 8).

**Still open:** whether xpac and deleted works stay in the served index, and whether the instance runs all the
time or only for demos.

---

## The whole stack

| Concern | Choice | Notes |
|---|---|---|
| HTTP API | **FastAPI** + **Uvicorn** | the only two Python packages added |
| Validation, caps | **FastAPI's `Query` constraints** | comes with FastAPI, no extra package |
| Concurrency limit | **anyio's `CapacityLimiter`**, 4 searches at a time | already installed with FastAPI; in use |
| Result cache | **`functools.lru_cache`** | stdlib; not added yet |
| Build + run | **Docker** multi-stage build, **Docker Compose** | compiles the C++ in a builder stage, ships a slim runtime; the index stays outside the image |
| TLS + reverse proxy | **Caddy**, as the second Compose service | automatic Let's Encrypt certificates, ~4 lines of config |
| Process supervision | **systemd** unit running Compose | already on the machine |
| Index storage | **S3**, copied to an EBS volume with the **AWS CLI** | CLI preinstalled on Amazon Linux |
| Machine | **one EC2 instance**, 16 GiB RAM | plus an AMI snapshot once it works |
| Shell access | **SSM Session Manager** | agent preinstalled, no SSH keys or port 22 |
| Logs | **journald** (`journalctl -u startorch`) | already there |
| Uptime check | a **free external monitor** hitting `/healthz` | UptimeRobot, Better Stack, or similar |
| Cost safety | **AWS Budgets** alarm | free, set it first |
| Load test | **the existing `bmw_performance.py`** | already written |

New things to install: two pip packages, plus Docker on the instance. Caddy arrives as an image rather than a
binary you manage. Everything else is the OS, AWS, or code you have.

---

## What this drops, and when to add it back

| Dropped | Why it's fine at small scale | Add it back when |
|---|---|---|
| Metadata store (titles, authors) | Return OpenAlex ids; the client resolves them from the OpenAlex API | you need titles without a second request, or results must work offline |
| A registry (ECR/GHCR), scikit-build-core | Build the image on the instance itself. Docker stays: it pins the build environment, and learning it is a goal of this project | you want builds off the box, or a second machine needs to pull the image |
| Terraform, Packer | One instance, set up once, captured as an AMI | you rebuild the environment more than a couple of times |
| ALB, ACM, Route 53 | Caddy terminates TLS on the box and renews certificates itself | you need more than one instance behind one name |
| WAF, CloudFront | In-app caps plus a cache; Cloudflare's free tier if abuse appears | you get real traffic, or real abuse |
| OpenTelemetry, CloudWatch agent, structlog | journald plus one uptime check | you need to debug slowness you cannot reproduce |
| GitHub Actions, OIDC deploys | `git pull && systemctl restart startorch` | more than one person deploys |
| Blue/green index refresh | Stop, swap, start: about a minute of downtime, quarterly | downtime becomes visible to someone who matters |
| Multiple workers, autoscaling | One process, one instance | one instance genuinely saturates |

**What this costs you:** a single point of failure, about a minute of downtime on each restart (the index load),
and no horizontal scale. All acceptable for a demo, none acceptable for a product.

---

## Step 1. Wrap `Searcher` in FastAPI — done

**Goal.** `GET /search?query=...&k=10` returns JSON, with the index loaded once.

**Built** (`python/src/startorch/api/`):
- The lifespan handler starts the index load as a background task. The `Searcher` lives in the module's
  `resources` dict. It is never built per request.
- `k` must be 1 to 100 (default 10), and the query 1 to 512 characters. FastAPI's `Query` constraints return 422
  for anything else.
- Three routes: `/search`, `/healthz` (process alive), `/readyz` (index loaded).
- Responses are Pydantic models in `schemas.py`. They include the C++ search time as `took_ms`.

**Not done yet.** Cache results with `@lru_cache(maxsize=1024)` on `(query, k)`. Real traffic repeats.

**Research.** FastAPI lifespan events, `Query` constraints, Pydantic response models, `functools.lru_cache`.

## Step 2. Concurrency — done

The bindings used to hold the GIL for a whole search, so one slow query blocked everything. Now:

1. **The GIL is released** in the bindings for the `QueryEngine` constructor, `query` and `query_exhaustive`.
2. **The C++ query path is `const`,** so the compiler rejects writes to shared state. `term_meta_mapping[term]`
   became a `find()` lookup, which is safe for concurrent reads.
3. **The tokenizer gives each thread its own DuckDB cursor.** The shared DuckDB connection was the real failure:
   322 of 400 threaded searches failed before this fix.
4. **Searches run on worker threads** through `anyio.to_thread.run_sync`, with a `CapacityLimiter` of 4.

**One change from the plan.** The endpoints stay `async def` and hand each search to a worker thread, instead of
becoming `def` endpoints. This gives searches their own limit, separate from Starlette's thread pool. Requests
beyond the limit wait for a slot instead of getting a 503.

**Checked by:**
- ThreadSanitizer on `QueryEngineConcurrencyTest`. 8 threads share one engine, and no data race is found.
- `test_search.py` and `test_tokenizer.py`. Threaded results match sequential ones, and another thread keeps
  running during a load and during a long search.

**Research.** Python GIL, `gil_scoped_release`, `const`-correctness, ThreadSanitizer, `anyio.to_thread`,
`CapacityLimiter`.

## Step 3. Results without a metadata store — done

The index holds ids and scores only. Rather than building a second database of titles, the API returns ids and
the caller resolves them. OpenAlex ids get their `W` prefix back, so they can go straight to the OpenAlex API;
MS MARCO ids stay bare.

To resolve them:

- `https://api.openalex.org/works?filter=ids.openalex:W1|W2|...` fetches up to 50 works in one request.
- Do it in the browser, or server-side in one call per search.

Two consequences to accept: results depend on OpenAlex being up, and ids deleted since the June 2026 snapshot
resolve to nothing (see `CLAUDE.md` — that already affects about a sixth of sampled W7 ids). Filtering those
out of the index is a separate, open decision.

**Research.** OpenAlex `ids.openalex` filter, the polite pool and its rate limits, batching lookups.

## Step 4. Build the image with Docker

**Goal.** One image that runs the same on your machine and on the instance.

**The Dockerfile, in two stages.**
- *Builder*: a base with a C++20 compiler, CMake and **uv**. Copy `cpp/` and `python/`, run
  `cmake -S cpp -B cpp/build && cmake --build cpp/build -j`, then `uv sync --frozen`. Add a CMake option to
  skip the GoogleTest fetch, so server builds don't download a test framework.
- *Runtime*: a slim Python base. Copy the built package and the `.so` from the builder. No compiler in the
  final image.

**Getting the layers right is most of the lesson.** Copy dependency manifests and run `uv sync` *before*
copying source, so editing a `.py` file doesn't reinstall every package. Put the C++ build after that, since it
changes less often than Python code but more often than dependencies.

**The index never goes in the image.** It is 56 GB, it changes on a different schedule, and an image is not a
data store. It lives on the host and is bind-mounted read-only (step 5).

**Two things that bite here specifically.**
- **Architecture.** Build for the architecture you deploy on. If you develop on x86_64 and deploy on Graviton,
  use `docker buildx` with `--platform`, and re-verify the index files, which are written in native byte order
  and native struct sizes.
- **Build time.** The C++ build inside a fresh image is minutes, not seconds. Use BuildKit's cache mounts for
  the CMake build directory if iteration gets painful.

**Research.** Docker multi-stage builds, layer caching and COPY order, `.dockerignore` (exclude `cpp/build/`,
`.venv/`, `python/.tmp/`), BuildKit cache mounts, `docker buildx --platform`, slim vs distroless base images.

## Step 5. Index to S3, then to the instance

- Upload the serving files (not `token_stream/`, not `partial/`) to a versioned prefix:
  `aws s3 sync <dir> s3://<bucket>/indexes/full-en/2026-09-19/`.
- On the instance, `aws s3 sync` it down to a gp3 volume. About 56 GB, so several minutes.
- Turn on **S3 versioning** and keep the previous build until the new one has served traffic.
- Snapshot the data volume once it is populated, so a rebuilt instance skips the download.

**Do not use Mountpoint for S3 or s3fs.** The engine memory-maps posting files and reads randomly; every page
fault would become a network round trip.

**Research.** `aws s3 sync`, S3 versioning, EBS gp3 throughput, EBS snapshots, VPC gateway endpoint for S3.

## Step 6. Run it with Compose, behind Caddy

**Two services in one `compose.yaml`.**
- `startorch`: your image, running Uvicorn. No published ports — only Caddy reaches it, by service name on the
  Compose network. Bind-mount the index read-only: `/data/index:/index:ro`.
- `caddy`: the official image, publishing 80 and 443, proxying to `startorch:8000`. Give it a named volume for
  `/data`, or it re-requests certificates on every restart and hits Let's Encrypt's rate limits.

**Memory limits are where containers get interesting for this service.** Set `mem_limit` above the 8.7 GiB load
peak, with headroom — 12 GiB on a 16 GiB machine is a reasonable start. Two things to understand before
choosing a number:
- Memory-mapped posting pages are **charged to the container's cgroup**, not treated as free host page cache.
  The kernel reclaims those clean pages before killing anything, so a query-heavy burst shows as cache churn
  rather than failure.
- Set the limit too close to the heap and the container is OOM-killed mid-load. That looks like a mysterious
  crash with no traceback; check `docker inspect` for `OOMKilled` before debugging anything else.

Also set `restart: unless-stopped`, and keep swap off for the container (`memswap_limit` equal to `mem_limit`).
Swapped heap hurts far more than evicted posting pages.

**Supervision.** A small systemd unit runs `docker compose up` on boot, so the stack survives a reboot without
Docker's restart policy being the only thing holding it together.

**Networking.** Point your domain's A record at the instance's Elastic IP. Security group: 80 and 443 open,
nothing else. Shell access through SSM Session Manager.

**Research.** Compose service networking and DNS by service name, bind mounts vs named volumes, `mem_limit`
and `memswap_limit`, cgroup v2 memory accounting for page cache, `OOMKilled` in `docker inspect`, Caddy's
`/data` volume, systemd units that wrap Compose.

## Step 7. Keep it from falling over — partly done

- **Done:** the input limits from step 1 (`k`, query length) and the search limit from step 2.
- **Not done:** a request timeout, so a pathological query cannot hold a slot forever.
- If abuse shows up, put **Cloudflare's free tier** in front: DNS, edge caching, and rate limiting at no cost,
  and it hides the origin address. That is the one dependency worth adding under pressure.

**Research.** Cloudflare proxied DNS, rate limiting rules, `Cache-Control`, request timeouts in Uvicorn.

## Step 8. Minimal operations

- **Logs:** `journalctl -u startorch -f`. Uvicorn's access log is enough to see traffic and errors.
- **Uptime:** a free external monitor pinging `/healthz` every few minutes, alerting by email.
- **Cost:** an AWS Budgets alarm, set before the first full-size instance runs overnight. A 16 GiB instance
  running 24/7 is the dominant cost; stopping it when not in use is the simplest saving, and an Elastic IP plus
  the EBS volume keep the setup intact while stopped.
- **Load test:** point the existing `bmw_performance.py` at the deployed URL's underlying profile, or use
  `hey`/`ab` against `/search`, before showing it to anyone.
- **Deploy:** `git pull`, `docker compose build`, `docker compose up -d`. Compose recreates only what changed.
  About a minute of downtime while the index reloads.
- **Container logs:** `docker compose logs -f startorch`, which journald still captures underneath.

---

## Anti-patterns to avoid

- **Memory-mapping posting files from S3** (Mountpoint, s3fs). Random access over the network.
- **Autoscaling.** A new instance needs tens of seconds and 56 GB before it can answer anything.
- **Several worker processes**, each loading its own 6.7 GiB.
- **Exposing Uvicorn directly.** Caddy in front; the app container publishes no ports.
- **Baking the index into the image.** 56 GB, a different release cadence, and every rebuild would copy it.
  Bind-mount it read-only.
- **Shipping `token_stream/` or `posting/partial/`.** Build inputs, several times the size of what serving needs.

## Decisions

1. **Decided:** release the GIL and search in parallel (step 2).
2. **Decided:** resolve ids on the client, with no metadata store (step 3).
3. **Open:** whether xpac and deleted works stay in the served index (see `CLAUDE.md`).
4. **Open:** whether the instance runs all the time, or only when you demo it.
