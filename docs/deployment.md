# Deploying Startorch as a live search service (EC2 + S3)

A plan to put the query engine behind a URL, sized for **a small number of users** — a portfolio demo, a few
friends, an occasional reviewer. Not a product launch.

Two rules shaped it: **write as little as possible**, and **depend on as little as possible**. Those pull
against each other, so where a managed service costs real money or real setup time, this picks the boring
option that ships.

Last updated 2026-09-26. The current status is in the next section. Check current docs before pinning versions.

---

## Status

**The whole site runs locally in Docker Compose, on full-en, behind Caddy. Nothing is deployed to AWS yet.**

| Step | Status | Notes |
|---|---|---|
| 1. FastAPI wrapper | Done | `/api/search`, `/api/healthz`, `/api/readyz`. No result cache yet. |
| 2. Concurrency | Done | The GIL is released. The engine and tokenizer are thread-safe. 4 searches run at a time. |
| 3. Results without a metadata store | Done | The API returns OpenAlex ids with the `W` prefix. The page fetches titles from OpenAlex. |
| 4. Front end | Done | One static page in `frontend/`, served by FastAPI in development and by Caddy under Compose. |
| 5. Docker image | Done | Two-stage `Dockerfile`, about 920 MB. Slimming it is optional. |
| 6. Index in S3 | Not started | The full-en and msmarco indexes are built in the current format, ready to upload. |
| 7. Compose + Caddy | Done locally | `compose.yaml`, `Caddyfile`, `.env`. Plain HTTP on port 80. Not on EC2 yet; no systemd unit, no domain. |
| 8. Protection | Partly done | Input limits and the search limit are in. No request timeout yet. |
| 9. Operations | Not started | The health endpoints are ready for an uptime monitor. |

### What works now

- **The search page** at `/`. It shows one card per result: title, authors, year, venue, citation count, and
  DOI / open-access links. Each title links to the work's OpenAlex page. Details are in step 4.
- **Endpoints.** `/api/search?query=&k=` returns `{query, k, corpus, took_ms, hits: [{id, score}]}`. `k` must be
  1 to 100, and the query 1 to 512 characters. Anything else returns 422.
- **Background loading.** The server starts at once and loads the index on a worker thread. `/api/healthz`
  answers during the load. `/api/readyz` and `/api/search` return 503 until the load finishes.
- **Failed load.** Both health checks return 503, because only a restart can fix it.
- **Searches off the event loop.** Each search runs on a worker thread, at most 4 at a time. A slow search does not
  block other requests, including `/api/healthz`.
- **Thread safety.**
  - In C++, the query path is `const`, so the compiler rejects any write to shared state.
  - ThreadSanitizer finds no data race when 8 threads share one engine.
  - The tokenizer gives each thread its own DuckDB cursor.
- **Tests.**
  - `uv run pytest` runs 94 Python tests in about 17 s, including the API and the served page.
  - `node --test frontend/tests/` runs the page's formatting tests.
  - `ctest` runs 198 C++ tests.
  - Browser behavior is covered by the manual checklist in `frontend/TESTING.md`.
- **Run it locally, without Docker.** From `python/`, run `STARTORCH_PROFILE=full-en fastapi dev src/startorch/api/api.py`,
  then open `http://127.0.0.1:8000/`. Without the variable, the profile is `msmarco`, whose results have no titles.
- **Run it locally, in Docker.** From the repository root, run `docker compose up -d --build`, then open
  `http://localhost/`. The profile and the index location come from `.env` (step 7).

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
| Search time, full-en, the page's example queries | 30 to 50 ms for 20 results |
| Search time, full-en, common-word queries | up to about 600 ms for 10 results |
| Time from opening a search link to results, during a full-en load | 17 s; the page waits and then searches by itself |
| full-en in Compose: time to ready, memory after load | about 16 s, 7.0 GiB |
| Docker image | about 920 MB |

So the target is one instance with 16 GiB of RAM, a 30 GB root volume (OS, Docker, images and build cache), and
an 80 GB gp3 data volume for the 56 GB index.

**Result quality on full-en.** Many queries return mostly OpenAlex expansion records: datasets, software, and
"other" items with no citations. Their title-only text is short, and BM25 favors short documents. For example,
"graph neural networks" and "Riemann hypothesis" have no scholarly work in their top 5. The page's example queries
were chosen from those whose top 10 are 9 or 10 scholarly works. PageRank is expected to push these records down.

### Next

1. Add a request timeout and the result cache (steps 1 and 8).
2. Upload the full-en index to S3, and run the Compose stack on EC2 (steps 6 and 7).
3. Add the systemd unit, so the stack comes back after a reboot (step 7).
4. Buy a domain, point it at the Elastic IP, and switch Caddy to HTTPS (step 7).
5. Set up the uptime monitor and the budget alarm (step 9).
6. Optional: slim the image (step 5).

**Still open:** whether the instance runs all the time, or only for demos.

---

## The whole stack

| Concern | Choice | Notes |
|---|---|---|
| HTTP API | **FastAPI** + **Uvicorn** | the only two Python packages added |
| Validation, caps | **FastAPI's `Query` constraints** | comes with FastAPI, no extra package |
| Concurrency limit | **anyio's `CapacityLimiter`**, 4 searches at a time | already installed with FastAPI; in use |
| Result cache | **`functools.lru_cache`** | stdlib; not added yet |
| Front end | **One static page**: HTML, plain JavaScript, **Pico.css** from a CDN | no build step, no framework, no `node_modules` |
| Build + run | **Docker** multi-stage build, **Docker Compose** | compiles the C++ in a builder stage, ships a slim runtime; the index stays outside the image |
| TLS, page, reverse proxy | **Caddy**, as the second Compose service | serves the page, proxies `/api` to FastAPI, and gets Let's Encrypt certificates itself |
| Process supervision | **systemd** unit running Compose | already on the machine |
| Index storage | **S3**, copied to an EBS volume with the **AWS CLI** | CLI preinstalled on Amazon Linux |
| Machine | **one EC2 instance**, 16 GiB RAM | plus an AMI snapshot once it works |
| Shell access | **SSM Session Manager** | agent preinstalled, no SSH keys or port 22 |
| Logs | **`docker compose logs`** | Docker keeps them; its `journald` log driver can send them to journald |
| Uptime check | a **free external monitor** hitting `/api/healthz` | UptimeRobot, Better Stack, or similar |
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

**Goal.** `GET /api/search?query=...&k=10` returns JSON, with the index loaded once.

**Built** (`python/src/startorch/api/`):
- The lifespan handler starts the index load as a background task. The `Searcher` lives in the module's
  `resources` dict. It is never built per request.
- `k` must be 1 to 100 (default 10), and the query 1 to 512 characters. FastAPI's `Query` constraints return 422
  for anything else.
- Three routes under `/api`: `/api/search`, `/api/healthz` (process alive), `/api/readyz` (index loaded). The
  prefix leaves `/` free for the page.
- Responses are Pydantic models in `schemas.py`. They include the C++ search time as `took_ms`, and `corpus`, so
  the page knows whether ids have OpenAlex records.
- The profile to serve comes from the `STARTORCH_PROFILE` environment variable, so one image can serve any index.

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

The page resolves them in the browser, with one request per results page:
`https://api.openalex.org/works?filter=ids.openalex:W1|W2|...&include_xpac=true`. Step 4 has the details.

Two consequences to accept: results depend on OpenAlex being up, and ids deleted since the June 2026 snapshot
resolve to nothing (see `CLAUDE.md` — that already affects about a sixth of sampled W7 ids). The page shows those
as muted fallback cards. Removing them from the ranking is left to PageRank.

**Research.** OpenAlex `ids.openalex` filter, OpenAlex rate limits and API keys, batching lookups.

## Step 4. Front end — done

**Goal.** A simple, presentable search page, without a framework or a build step.

**Built** (`frontend/`):
- `index.html`: a header with links to GitHub and the technical report, the search box, four example queries, the
  results list, and a short "How it works" section. Styled with Pico.css from a CDN.
- `format.js`: pure formatting logic (titles, authors, the meta line, the OpenAlex request). No DOM, so Node tests
  it: `node --test frontend/tests/`.
- `app.js`: the page logic. Everything from the network goes in with `textContent`, never `innerHTML`.
- `style.css`: small additions on top of Pico.

**How a search works.**
1. The page calls `/api/search` and shows one card per hit at once, in rank order.
2. It makes one request to the OpenAlex API for all the cards, with `include_xpac=true`. Without that flag,
   OpenAlex leaves out its expansion records, which are about 42% of the English corpus.
3. It fills each card: title, authors, year, venue, citation count, DOI and open-access links. OpenAlex returns
   records in its own order, so they are matched back to the ranked list by id.

**Rules.**
- **No abstracts.** Each card shows the title as its main text.
- **`[Untitled]`** replaces a missing, empty, blank, or markup-only title.
- **Deleted records** get a muted card: "No longer in OpenAlex: deleted or merged since the June 2026 snapshot".
- **If OpenAlex is unreachable,** the cards keep their rank and id, link to OpenAlex, and explain the problem.

**Page states.**
- While the index loads, the page says so, polls `/api/readyz`, and runs the search when ready.
- If the backend is unreachable, it says the demo may be offline.
- Invalid queries and queries with no results get a clear message.
- The query is in the URL (`/?q=...`), so a search can be shared. Back and forward work.
- Starting a new search cancels the previous one, so old results never replace newer ones.

**OpenAlex limits.** Without a key, OpenAlex allows 1,000 calls per day. Since the browser makes the calls, each
visitor has their own allowance, and a search costs one call. A free API key raises the limit to $1 of usage per
day. But a key is tied to your account's budget, so it must never go in browser code; using one would mean
moving the lookups to the server.

**Serving.** In development, FastAPI serves `frontend/` at `/`. In production, Caddy serves it and sends only
`/api/*` to FastAPI (step 7). The page calls `/api/...` on its own origin, so it needs no CORS setup and no change
between the two.

**Research.** Pico.css, `fetch` and `AbortController`, `history.pushState`, `URLSearchParams`, `node:test`.

## Step 5. Build the image with Docker — done

**Goal.** One image that runs the same on your machine and on the instance.

**Built** (`Dockerfile` and `.dockerignore`, at the repository root). Two stages, both on `python:3.14-slim`:
- *Build stage.* Installs a compiler, CMake, git, and uv (pinned, through pip). Then three layers, in this order:
  1. `pyproject.toml` and `uv.lock` only, then `uv sync --frozen --no-dev --no-install-project`: the
     dependencies.
  2. `cpp/`, then CMake builds only the `startorch_cpp` target. The `.so` lands in `python/src/startorch/_native/`.
  3. `python/` and `project-config.toml`, then `uv sync --frozen --no-dev` again, to install the package itself.
- *Runtime stage.* Copies `python/` (the venv, the source and the `.so`) and `project-config.toml` from the build
  stage. No compiler. It also installs DuckDB's `fts` extension and copies `frontend/`.
- `CMD` runs one Uvicorn process on `0.0.0.0:8000`.

**Layer order is most of the lesson.** Dependencies change least, then C++, then Python. So editing a `.py` file
rebuilds only the last layers, and the C++ build is reused.

**The index never goes in the image.** It is 56 GB, it changes on a different schedule, and an image is not a
data store. It lives on the host and is bind-mounted read-only at `/data/scholar_rank`, because every path in
the baked-in `project-config.toml` starts there (steps 6 and 7).

**The page is in the image, for plain `docker run`.** Under Compose, Caddy serves its own copy of `frontend/`
(step 7), and the app never sees page requests.

**What went wrong on the way, and the fixes.**

| Symptom | Cause | Fix |
|---|---|---|
| `ModuleNotFoundError: No module named 'startorch'` | The first `uv sync` installs dependencies only | A second `uv sync` after copying the source |
| `ImportError: GLIBCXX_3.4.36 not found` | In `.dockerignore`, `*.so` matches only the context root. The host's `.so`, built by a newer GCC, was copied over the one built in the image | `**/*.so` |
| Searches return 500 when the container has no network | DuckDB downloads `fts` (the query stemmer) on the first search, into the container | `INSTALL fts` in the runtime stage |
| full-en is OOM-killed, even with `-m 16g` | Docker Desktop runs containers in a VM, which had 7.4 GiB. `-m` cannot go above it | Raise Docker Desktop's memory (now 11.67 GiB). EC2 has no such VM |
| `/` returns 404 | `frontend/` was not in the runtime stage | `COPY frontend/ frontend/` |

**Not done yet, all optional.**
- A CMake option to skip the GoogleTest fetch. It costs build time only.
- A non-root `USER`. Run `INSTALL fts` after it, so the extension lands in that user's home.
- Move analysis-only packages (pandas, matplotlib, ipykernel, tabulate) into a dependency group, which
  `--no-dev` skips. Largest packages now: pyarrow 152 MB, duckdb 58 MB, pandas 42 MB, jedi 33 MB, matplotlib 28 MB.
- Exclude `frontend/tests/` and `frontend/TESTING.md` in `.dockerignore`. Under plain `docker run`, FastAPI serves
  them. Caddy already hides them (step 7).

**Two things that bite here specifically.**
- **Architecture.** Build for the architecture you deploy on. If you develop on x86_64 and deploy on Graviton,
  use `docker buildx` with `--platform`, and re-verify the index files, which are written in native byte order
  and native struct sizes.
- **Build time.** The C++ build inside a fresh image is minutes, not seconds. Use BuildKit's cache mounts for
  the CMake build directory if iteration gets painful.

**Research.** Docker multi-stage builds, layer caching and COPY order, `.dockerignore` (exclude `cpp/build/`,
`.venv/`, `python/.tmp/`), BuildKit cache mounts, `docker buildx --platform`, slim vs distroless base images.

Running the image alone, without Compose, serves the API and the page on `http://localhost:8000/`:
```
docker run -p 8000:8000 -e STARTORCH_PROFILE=full-en -v /data/scholar_rank:/data/scholar_rank:ro startorch
```

When a container exits, `docker inspect <name> --format '{{.State.ExitCode}} {{.State.OOMKilled}}'` tells a crash
from an OOM kill (exit code 137 with `OOMKilled=true`), and `docker logs <name>` shows its last output.

## Step 6. Index to S3, then to the instance

- Upload the serving files (not `token_stream/`, not `posting/partial/`) to a versioned prefix:
  ```
  aws s3 sync /data/scholar_rank/posting/full-en s3://<bucket>/indexes/full-en/2026-09-19/ \
      --exclude "token_stream/*" --exclude "posting/partial/*"
  ```
- On the instance, `aws s3 sync` it down to a gp3 volume. About 56 GB, so several minutes.
- **Keep the layout.** The image reads its paths from `project-config.toml`, all under `/data/scholar_rank`.
  Serving full-en needs `posting/full-en/posting/` (49 GiB, without `partial/`) and `posting/full-en/lookup/`
  (3.9 GiB).
  So download into `<volume>/posting/full-en/`, and set `INDEX_ROOT=<volume>` in `.env` (step 7). Compose
  then mounts `<volume>` at `/data/scholar_rank` in the container.
- Turn on **S3 versioning** and keep the previous build until the new one has served traffic.
- Snapshot the data volume once it is populated, so a rebuilt instance skips the download.

**Do not use Mountpoint for S3 or s3fs.** The engine memory-maps posting files and reads randomly; every page
fault would become a network round trip.

**Research.** `aws s3 sync`, S3 versioning, EBS gp3 throughput, EBS snapshots, VPC gateway endpoint for S3.

## Step 7. Run it with Compose, behind Caddy — done locally

**Built** (`compose.yaml`, `Caddyfile`, and a gitignored `.env`, at the repository root). Tested locally on
full-en on 2026-09-26. Not on EC2 yet.

**Two services in `compose.yaml`.**
- `startorch`: builds the image from the `Dockerfile`'s `runtime` stage.
  - `STARTORCH_PROFILE` comes from `.env`, with `msmarco` as the default.
  - The index is bind-mounted read-only: `${INDEX_ROOT:-/data/scholar_rank}:/data/scholar_rank:ro`. The right
    side is fixed by `project-config.toml`; only the left side changes between machines.
  - `mem_limit` and `memswap_limit` are both 12g, and `restart: unless-stopped`.
  - No published ports. Only Caddy reaches it, at `startorch:8000`: on the Compose network, a service's name is
    its hostname.
- `caddy`: the official `caddy:2` image, publishing port 80.
  - Bind mounts, read-only: `./Caddyfile` at `/etc/caddy/Caddyfile`, the path the image's default command reads,
    and `./frontend` at `/srv/frontend`. Compose resolves `./` against the folder holding `compose.yaml`, so these
    work wherever the repository is cloned.
  - Named volumes: `caddy_data` at `/data`, which holds certificates, and `caddy_config` at `/config`. Without
    `caddy_data`, every recreated container requests new certificates and hits Let's Encrypt's rate limits.

**`.env`, one per machine.** Compose reads it only to fill in `${...}` in `compose.yaml`. The `environment:` key
is what passes the profile into the container. Use `KEY=value`, which systemd's `EnvironmentFile` and
`docker run --env-file` also read:

```
STARTORCH_PROFILE=full-en
INDEX_ROOT=/data/scholar_rank
```

**Run and check it.**

```
docker compose up -d --build
docker compose logs -f startorch      # wait for "Index for profile full-en is loaded"
```

| Check (2026-09-26, local, full-en) | Result |
|---|---|
| `/api/readyz` through Caddy | 200 after about 16 s |
| A search through Caddy | 30 to 40 ms |
| `/`, `/app.js`, `/style.css` | 200, served by Caddy |
| `/TESTING.md`, `/tests/format.test.js` | 404 |
| `localhost:8000` | Unreachable: the app publishes no port |
| Memory | App 7.0 GiB, Caddy 36 MiB, no OOM kill |

**The Caddyfile, before you have a domain.** Caddy serves plain HTTP on port 80. On EC2, that is the instance's
Elastic IP, for example `http://203.0.113.10/`:

```
:80 {
    @hidden path /tests/* /TESTING.md

    handle @hidden {
        respond 404
    }
    handle /api/* {
        reverse_proxy startorch:8000
    }
    handle {
        root * /srv/frontend
        file_server
    }
}
```

- **`:80`** is the site address: any hostname, port 80. It decides whether Caddy uses HTTPS.
- **`@hidden`** is a named matcher. It names a condition once, here two paths, for directives to use.
- **The `handle` blocks** are mutually exclusive: a request runs only the first one that matches, and the block
  with no matcher always goes last. The hidden files get a 404, `/api/*` goes to the app, and everything else is
  a file from `/srv/frontend`.
- **Inside the last block**, `root` sets the folder and `file_server` serves from it, including `index.html` for `/`.

This is enough to test the whole site on EC2. But there is no HTTPS: a normal Let's Encrypt certificate needs a
domain name. Browsers mark the page "Not secure", so do not send this address to recruiters.

**Two things that bit here.**
- **Caddy sorts directives.** It does not run a site block's directives in the order written. A bare
  `respond @hidden 404` next to the `handle` blocks runs after them, so the files were still served. Inside its
  own `handle` block, it works.
- **Editing a single-file bind mount.** The mount is tied to the file's inode. Many editors save by writing a new
  file, and the container then keeps the old content, so `caddy reload` reloads the old file. After editing the
  Caddyfile, run `docker compose up -d --force-recreate caddy`. Mounting a folder instead of the file avoids this.

**The Caddyfile, once you have a domain.**
1. Buy a domain from any registrar (Route 53, Cloudflare, Porkbun, and others all work).
2. Add a DNS A record: `search.yourdomain.com` → the Elastic IP.
3. In the Caddyfile, replace `:80` with the name. Keep the matcher and the three `handle` blocks the same:

   ```
   search.yourdomain.com {
       ...
   }
   ```
4. In `compose.yaml`, publish `"443:443"` on the `caddy` service too, and `"443:443/udp"` for HTTP/3.
5. Recreate Caddy: `docker compose up -d --force-recreate caddy`.

Caddy then gets a Let's Encrypt certificate by itself, renews it, and redirects HTTP to HTTPS. Keep ports 80
and 443 open: port 80 answers the certificate check and serves the redirect.

Nothing else changes. The page calls `/api/...` on its own origin, so neither the page nor the API needs editing.

**Use an Elastic IP.** A normal EC2 public IP changes every time the instance stops. An Elastic IP stays the same,
so the DNS record never needs updating, even if the instance only runs for demos. AWS charges a small hourly fee
for every public IPv4 address, in use or not; check current pricing.

**Optional: HTTPS before buying a domain.** A wildcard DNS service such as sslip.io maps a name like
`203-0-113-10.sslip.io` to that IP, and Caddy can get a real certificate for it. This is useful for testing
HTTPS, but these names share Let's Encrypt's rate limits with every other user, and a real domain looks better.

**Memory limits are where containers get interesting for this service.** Set `mem_limit` above the 8.7 GiB load
peak, with headroom — 12 GiB on a 16 GiB machine is a reasonable start. Two things to understand before
choosing a number:
- Memory-mapped posting pages are **charged to the container's cgroup**, not treated as free host page cache.
  The kernel reclaims those clean pages before killing anything, so a query-heavy burst shows as cache churn
  rather than failure.
- Set the limit too close to the heap and the container is OOM-killed mid-load. That looks like a mysterious
  crash with no traceback; check `docker inspect` for `OOMKilled` before debugging anything else.
- On Docker Desktop, containers run inside a VM, and the VM's memory is the real ceiling. A `mem_limit` above
  it does nothing. Raise it under Settings → Resources. On EC2, Docker runs on the host directly.

Also set `restart: unless-stopped`, and keep swap off for the container (`memswap_limit` equal to `mem_limit`).
Swapped heap hurts far more than evicted posting pages.

**Supervision, not done yet.** A small systemd unit runs `docker compose up` on boot, so the stack survives a
reboot without Docker's restart policy being the only thing holding it together.

**Networking.** Security group: 80 and 443 open, nothing else. Shell access through SSM Session Manager.

**Research.** Compose service networking and DNS by service name, bind mounts vs named volumes, `mem_limit`
and `memswap_limit`, cgroup v2 memory accounting for page cache, `OOMKilled` in `docker inspect`, Caddy's
`/data` volume, Caddyfile `handle` and `file_server`, Elastic IPs, DNS A records, systemd units that wrap Compose.

## Step 8. Keep it from falling over — partly done

- **Done:** the input limits from step 1 (`k`, query length) and the search limit from step 2.
- **Not done:** a request timeout, so a pathological query cannot hold a slot forever.
- If abuse shows up, put **Cloudflare's free tier** in front: DNS, edge caching, and rate limiting at no cost,
  and it hides the origin address. That is the one dependency worth adding under pressure.

**Research.** Cloudflare proxied DNS, rate limiting rules, `Cache-Control`, request timeouts in Uvicorn.

## Step 9. Minimal operations

- **Logs:** `docker compose logs -f startorch`. Uvicorn's access log is enough to see traffic and errors. Docker
  keeps logs in its own files, so cap them with the `logging:` key's `max-size`, or switch to the `journald` log
  driver to read them with `journalctl`.
- **Uptime:** a free external monitor pinging `/api/healthz` every few minutes, alerting by email.
- **Cost:** an AWS Budgets alarm, set before the first full-size instance runs overnight. A 16 GiB instance
  running 24/7 is the dominant cost; stopping it when not in use is the simplest saving, and an Elastic IP plus
  the EBS volume keep the setup intact while stopped.
- **Load test:** point the existing `bmw_performance.py` at the deployed URL's underlying profile, or use
  `hey`/`ab` against `/api/search`, before showing it to anyone. Then run the checklist in `frontend/TESTING.md`
  against the public address.
- **Deploy:** `git pull`, `docker compose build`, `docker compose up -d`. Compose recreates only what changed.
  About a minute of downtime while the index reloads.

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
3. **Decided:** a static page with no framework, served by Caddy on the same origin as the API. Cards show titles,
   with no abstracts (steps 4 and 7).
4. **Decided:** xpac and deleted works stay in the index for now. PageRank should push them down, and the page
   shows fallback cards for deleted ones (steps 3 and 4).
5. **Decided:** Compose runs the app and Caddy. Caddy serves the page, so the page stays up when the app is
   down, and it will handle HTTPS once there is a domain (step 7).
6. **Decided:** the profile and the index location come from `.env`, so one `compose.yaml` serves every machine
   (step 7).
7. **Decided:** the page is also in the image, so plain `docker run` serves the whole site (step 5).
8. **Open:** whether the instance runs all the time, or only when you demo it.
