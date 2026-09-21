# Deploying Startorch as a live search service (EC2 + S3)

A plan to put the query engine behind a URL, sized for **a small number of users** — a portfolio demo, a few
friends, an occasional reviewer. Not a product launch.

Two rules shaped it: **write as little as possible**, and **depend on as little as possible**. Those pull
against each other, so where a managed service costs real money or real setup time, this picks the boring
option that ships.

Written against the project state on 2026-09-20. Check current docs before pinning versions.

---

## The whole stack

| Concern | Choice | Notes |
|---|---|---|
| HTTP API | **FastAPI** + **Uvicorn** | the only two Python packages added |
| Validation, caps | **FastAPI's `Query` constraints** | comes with FastAPI, no extra package |
| Concurrency limit | **`threading.Semaphore`** or anyio's limiter | stdlib, or already installed with FastAPI |
| Result cache | **`functools.lru_cache`** | stdlib |
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

## Where the project stands today

**What already works**

- `Searcher` loads one index and answers queries in-process (`python/src/startorch/lexical/search.py`).
- `QueryEngine` (C++, pybind11) does Block-Max WAND top-k BM25 and returns `(score, mapped_id)`.
- `DocIdLookup` maps those back to OpenAlex ids.

**What serving still needs**

| Gap | Why it matters |
|---|---|
| No HTTP layer | Nothing accepts a request. `cli.py` prints to stdout. |
| The bindings never release the GIL | Two requests cannot search at the same time. See step 2. |
| No limits | `k`, query length and request rate are unbounded. |

**Numbers that drive the sizing** (measured 2026-09-19, full-en)

| Quantity | Value |
|---|---|
| Files needed to serve | `posting_*.bin` 47.7 GB, `block_meta.bin` 2.25 GB, `doc_len_list.bin` 1.73 GB, `doc_id_lookup.bin` 4.15 GB, metadata — **about 56 GB** |
| Files *not* needed to serve | `token_stream/` (~220 GB), `posting/partial/` — build inputs only |
| Heap after load | 6.7 GiB |
| Heap peak during load | 8.7 GiB |
| Load time | 14 s warm; longer from a cold disk |

So: one instance, 16 GiB of RAM, about 70 GB of disk, and a slow start.

---

## Step 1. Wrap `Searcher` in FastAPI

**Goal.** `GET /search?q=...&k=10` returns JSON, index loaded once at startup.

- Build the `Searcher` in FastAPI's **lifespan handler** and keep it on `app.state`. Never per request.
- `k: int = Query(10, ge=1, le=100)` and a max query length. Validation and the 422 response are free.
- Three routes: `/search`, `/healthz` (process alive), `/readyz` (index loaded).
- Return the search time the engine already reports, so latency is visible without a profiler.
- Cache with `@lru_cache(maxsize=1024)` on `(query, k)`. Real traffic repeats.

**Research.** FastAPI lifespan events, `app.state`, `Query` constraints, `functools.lru_cache`.

## Step 2. Decide how concurrency works

The one thing no tool decides for you. `cpp/python/bindings.cpp` binds `QueryEngine::query` without
`py::call_guard<py::gil_scoped_release>`, so the GIL is held for the whole search and one slow query blocks
everything.

At this scale the cheapest correct answer is:

1. **Add the GIL release** to the `query` bindings — one line of pybind11. First review whether
   `QueryEngine::query` is safe on several threads: its methods are not `const` and they share the mapped
   files and the doc-length vector.
2. **Write the endpoint as `def`, not `async def`.** Starlette then runs it in its threadpool, so searches
   overlap with no threading code.
3. **Bound it with a `Semaphore`** of two or three, and return 503 when full. A queue that grows without limit
   just turns into timeouts.

If the review in (1) turns up shared mutable state, skip it: keep one search at a time behind the semaphore
and accept the throughput. At this scale that is a real option, not a compromise.

**Research.** Python GIL, `gil_scoped_release`, thread safety vs `const`-correctness, Starlette threadpool,
`asyncio`/`anyio` timeouts.

## Step 3. Results without a metadata store

The index holds ids and scores only. Rather than building a second database of titles, return ids and let the
caller resolve them:

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

## Step 7. Keep it from falling over

- Caps from step 1 (`k`, query length) plus the semaphore from step 2 do most of the work.
- Add a request timeout so a pathological query cannot hold a slot forever.
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

## Decisions to make before writing code

1. GIL release, or one search at a time behind a semaphore (step 2).
2. Client-side id resolution, or a metadata store after all (step 3).
3. Whether xpac and deleted works stay in the served index (see `CLAUDE.md`).
4. Instance running 24/7, or started when you need to demo it.
