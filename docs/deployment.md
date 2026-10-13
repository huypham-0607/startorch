# Deploying Startorch as a live search service (EC2 + S3)

How the query engine is served at a public URL. It is sized for **a small number of users**: a portfolio demo, a
few friends, an occasional reviewer. Two rules shape it: **write as little as possible**, and **depend on as
little as possible**.

Last updated 2026-10-12. Future work and known problems are in [Open issues](#open-issues).

**This file holds no account details.** `<bucket>` and `<instance-id>` stand in for the real names. Account ids,
the bucket name, the instance id and the IP address stay out of the repository.

---

## Status

**Deployed, as a demo.** The site runs on one EC2 instance at `https://startorch.halzyonnn.dev`, on the full
English corpus. The instance is stopped most of the time, because running it all month costs about $95. The site
answers only while the instance runs.

| Step | Status |
|---|---|
| 1. FastAPI wrapper | Done |
| 2. Concurrency | Done |
| 3. Results without a metadata store | Done |
| 4. Front end | Done |
| 5. Docker image | Done |
| 6. Index in S3 | Done |
| 7. Compose + Caddy | Done |
| 8. Deploy to EC2 | Done |
| 9. Protection | Input limits and the search limit are in. |
| 10. Operations | The stop alarm is in place. No uptime monitor, by choice. |

**Run a demo.** Start the instance, wait 1 to 2 minutes, and open the site. Stop it afterwards (step 10).

**Run it locally.**
- In Docker: from the repository root, `docker compose up -d --build`, then open `http://localhost/`.
- Without Docker: from `python/`, `STARTORCH_PROFILE=full-en fastapi dev src/startorch/api/api.py`, then open
  `http://127.0.0.1:8000/`. Without the variable, the profile is `msmarco`, whose results have no titles.

**Tests.** `uv run pytest` (94 tests, including the API and the served page), `node --test frontend/tests/`,
`ctest` (198 tests), and the manual browser checklist in `frontend/TESTING.md`.

### Measurements (full-en)

| Quantity | Value |
|---|---|
| Files needed to serve | 53 files, 52.0 GiB: posting files, the id lookup, block metadata, document lengths |
| Memory | 6.7 GiB after the load (7.0 GiB in the container), 8.7 GiB peak while loading |
| Load time | 14 to 16 s with a warm cache |
| Search time | 30 to 50 ms for the page's example queries; up to about 600 ms for common words |
| Parallel search speedup | 1.7× on 2 threads, 2.7× on 4, 3.6× on 8 |
| Docker image | About 920 MB |
| msmarco, for comparison | 0.4 s load, 0.3 GiB |

## The stack

| Concern | Choice |
|---|---|
| HTTP API | FastAPI + Uvicorn, one process |
| Page | Static HTML, plain JavaScript, Pico.css from a CDN |
| Titles and metadata | The OpenAlex API, called from the browser |
| Build and run | Docker multi-stage build, Docker Compose |
| Page, proxy, HTTPS | Caddy, as the second Compose service |
| Index storage | S3, copied once to the instance's disk with the AWS CLI |
| Machine | One EC2 instance, reached through SSM Session Manager |
| Supervision | Docker's restart policy (`restart: unless-stopped`) |
| Cost control | An AWS Budgets alarm, and a CloudWatch alarm that stops a forgotten instance |

---

## Step 1. FastAPI wrapper

`python/src/startorch/api/`:
- The lifespan handler loads the index once, as a background task, so the server answers at once.
- Routes, all under `/api`, so `/` is free for the page:
  - `/api/search?query=&k=` returns `{query, k, corpus, took_ms, hits: [{id, score}]}`. `k` must be 1 to 100,
    and the query 1 to 512 characters. Anything else returns 422.
  - `/api/healthz`: the process is alive. It answers during the load.
  - `/api/readyz`: the index is loaded. It and `/api/search` return 503 until then.
- A failed load makes both health checks return 503, since only a restart fixes it.
- `STARTORCH_PROFILE` picks the index, so one image serves any profile. The default is `msmarco`.

## Step 2. Concurrency

- The bindings release the GIL while loading and searching.
- The C++ query path is `const`, so the compiler rejects writes to shared state. ThreadSanitizer finds no data
  race with 8 threads sharing one engine.
- The tokenizer gives each thread its own DuckDB cursor.
- Each search runs on a worker thread (`anyio.to_thread.run_sync`), at most 4 at a time (`CapacityLimiter`).
  Extra requests wait for a slot. A slow search never blocks the event loop.

## Step 3. Results without a metadata store

The index holds ids and scores only. The API returns OpenAlex ids with their `W` prefix, and the page resolves
them with one OpenAlex request per search:
`https://api.openalex.org/works?filter=ids.openalex:W1|W2|...&include_xpac=true`. MS MARCO ids stay bare.

## Step 4. Front end

`frontend/`: `index.html`, `app.js` (page logic), `format.js` (pure formatting, tested with Node), `style.css`.
- The page shows one card per hit at once, in rank order. One OpenAlex request then fills them: title, authors,
  year, venue, citation count, DOI and open-access links.
- `include_xpac=true` is required. Without it, OpenAlex hides its expansion records, about 42% of the corpus.
- A missing title shows `[Untitled]`. Works deleted since the snapshot get a muted card. If OpenAlex is down,
  the cards keep their rank and id.
- The query is in the URL (`/?q=`), so a search can be shared. While the index loads, the page waits, then
  searches by itself.
- The page calls `/api/...` on its own origin, so it works unchanged behind FastAPI or Caddy.

## Step 5. Docker image

`Dockerfile` and `.dockerignore`, at the repository root. Two stages, both on `python:3.14-slim`:
- **Build.** A compiler, CMake, git, and uv. Then three layers, the least-changing first: the dependencies, the
  C++ module (the `startorch_cpp` target only), and the Python source.
- **Runtime.** Copies `python/` (the venv, the source and the `.so`) and `project-config.toml`. No compiler. It
  also installs DuckDB's `fts` extension and copies `frontend/`, so plain `docker run` serves the whole site.
- `CMD` runs one Uvicorn process on `0.0.0.0:8000`.

The index is never in the image. It is bind-mounted read-only at `/data/scholar_rank`, where every path in
`project-config.toml` starts.

**Notes.**
- `.dockerignore` needs `**/*.so`. A bare `*.so` matches only the context root, so the host's `.so` would
  replace the one built in the image.
- `fts` must be installed at build time. Otherwise DuckDB downloads it on the first search.
- Docker Desktop runs containers in a VM, and the VM's memory is the real limit. EC2 has no such VM.
- When a container dies, `docker inspect <name> --format '{{.State.ExitCode}} {{.State.OOMKilled}}'` tells an
  OOM kill (137, `true`) from a crash.

## Step 6. Index in S3

S3 holds the one copy of the index outside your machine. The instance downloads it once.

- **One bucket for every artifact,** with a folder per capability and build date:
  `lexical/<profile>/<build-date>/` now, `graph/` and `semantic/` later. A bucket costs nothing by itself, and a
  new build never overwrites the one being served.
- **Settings:** the instance's region, public access blocked, ACLs disabled, versioning on, SSE-S3 encryption.
- **One lifecycle rule:** delete incomplete multipart uploads after 7 days, and old versions after 30 days. A
  failed upload otherwise leaves hidden parts that are billed forever.
- **Upload only the serving files.** `token_stream/` (220 GB) and `posting/partial/` are build inputs.

```
aws s3 sync /data/scholar_rank/posting/full-en s3://<bucket>/lexical/full-en/2026-09-19/ \
    --exclude "token_stream/*" --exclude "posting/partial/*"
```

Add `--dryrun` to see the file list first. Run it again after the upload: when it prints nothing, every file is
in the bucket. If the upload stops, the same command continues where it stopped.

**Never mount S3 as a file system** (Mountpoint, s3fs). The engine memory-maps posting files and reads them
randomly, so every page fault would become a network round trip.

## Step 7. Compose + Caddy

`compose.yaml`, `Caddyfile`, and a gitignored `.env`, at the repository root. The same two committed files run on
every machine. Only `.env` differs.

**`compose.yaml`, two services.**
- `startorch`: the image's `runtime` stage.
  - The index: `${INDEX_ROOT:-/data/scholar_rank}:/data/scholar_rank:ro`. The left side is the folder on this
    machine. The right side is fixed, so the code and `project-config.toml` never change.
  - `mem_limit` and `memswap_limit` are both 12g; `restart: unless-stopped`.
  - No published ports. Caddy reaches it at `startorch:8000`: on the Compose network, a service's name is its
    hostname.
- `caddy`: `caddy:2`, publishing TCP 80, TCP 443 and UDP 443.
  - Read-only bind mounts: `./Caddyfile` and `./frontend`.
  - Named volumes: `caddy_data` (certificates) and `caddy_config`.

**`.env`, one per machine.** Compose reads it to fill in `${...}` in `compose.yaml`. A value reaches a container
only through that service's `environment:` key.

| Variable | Your machine | The EC2 instance |
|---|---|---|
| `STARTORCH_PROFILE` | `full-en` | `full-en` |
| `INDEX_ROOT` | `/data/scholar_rank` | `/srv/index` |
| `SITE_ADDRESS` | not set, so `:80` | `startorch.halzyonnn.dev` |

**The Caddyfile.**

```
{$SITE_ADDRESS::80} {
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

- **The site address decides the protocol.** `:80` means plain HTTP for any hostname. A bare domain name means
  HTTPS: Caddy gets a Let's Encrypt certificate, renews it, and redirects HTTP to HTTPS.
- `@hidden` is a named matcher. It keeps the test files in `frontend/` off the site.
- The `handle` blocks are mutually exclusive. A request runs only the first match, and the block with no matcher
  always goes last.

**Notes.**
- **Never set `SITE_ADDRESS` to the domain on your own machine.** Caddy would ask Let's Encrypt for a certificate
  it cannot get, and each failure counts toward the limit of 5 per hostname per hour.
- **Caddy sorts directives** into a fixed order. A bare `respond` beside `handle` blocks runs after them, so put
  it inside its own `handle`.
- **After editing the Caddyfile, recreate Caddy:** `docker compose up -d --force-recreate caddy`. A single-file
  bind mount follows the file's inode, and editors often save a new file, so `caddy reload` reloads the old one.
- **Keep `caddy_data`.** `docker compose down -v` deletes it, and Let's Encrypt allows 5 certificates per week for
  one name.
- **Memory.** `mem_limit` must stay above the 8.7 GiB load peak, or the container dies during the load with no
  traceback.

## Step 8. Deploy to EC2

This is the largest step. One instance runs the Compose stack from step 7, and HTTPS comes from Caddy.

```
browser ──HTTPS──> startorch.halzyonnn.dev ──DNS A record──> Elastic IP
                                                                │
                              EC2 instance (r6a.large, us-east-2, Ubuntu)
                              ├── caddy       ports 80 and 443: the page, the /api proxy, the certificate
                              └── startorch   port 8000, not published
                                    └── /srv/index, read-only  <── downloaded once from S3
```

### 8.1 Who can do what

Three identities, each with only the access it needs.

| Identity | Used from | Credentials | Allowed to |
|---|---|---|---|
| Console admin, in the `admins` group | The browser | Password and MFA, no access keys | Everything. It does all the one-time setup. |
| `startorch-cli` user | The AWS CLI on your machine, profile `startorch` | One access key, in `~/.aws/credentials` | List the bucket, read and write its objects (no delete), look up instances, and start or stop instances tagged `project=startorch` |
| `startorch-instance` role | The instance | Short-lived, given by AWS automatically | Session Manager (`AmazonSSMManagedInstanceCore`), and read-only access to the bucket |

- **Never use root keys.** Root is needed once: to let IAM users see Billing.
- **The access key file is not encrypted,** which is why `startorch-cli` is narrow. If the key leaks, it cannot
  delete data, create resources, or open a shell.
- **The instance stores no keys.** Its role gives it credentials, so `aws s3 sync` works there with no setup.
- **The CLI needs the profile named:** `--profile startorch`, or `AWS_PROFILE=startorch`. There is no default
  profile, so a command without it fails, and cannot act as the wrong identity.

### 8.2 Network

**Security group** `startorch-web`, in the default VPC. It is the instance's firewall.

| Direction | Rule | Why |
|---|---|---|
| Inbound | TCP 80 from anywhere | The certificate check, and the redirect to HTTPS |
| Inbound | TCP 443 from anywhere | The site |
| Inbound | UDP 443 from anywhere | HTTP/3; optional |
| Outbound | All traffic | S3, Docker Hub, GitHub, Let's Encrypt, and Session Manager |

There is no port 22 and no port 8000. Replies need no rule: a security group allows them automatically.

**Elastic IP.** A normal public IP changes on every stop and start. An Elastic IP stays the same, so the DNS
record is set once. It is billed all month, running or not.

**DNS.** At the registrar, one `A` record: host `startorch`, answer the Elastic IP, TTL 600. It must be an `A`
record; a `CNAME` points at a name, not an address.

**`.dev` is HTTPS-only.** Browsers refuse plain HTTP for every `.dev` domain. The site opens in a browser only
after Caddy has its certificate.

### 8.3 The instance

| Setting | Value | Why |
|---|---|---|
| Region | `us-east-2`, the bucket's region | The S3 download is free within a region |
| Image | Ubuntu Server 26.04 LTS, x86_64 | The index files were built and tested on x86_64 |
| Type | `r6a.large`: 2 vCPU, 16 GiB | The cheapest x86_64 type with 16 GiB. The load peaks at 8.7 GiB. |
| Storage | One 80 GB gp3 volume, deleted on termination | It holds the OS, Docker and the index. A terminated instance leaves nothing to pay for. |
| Key pair | None | The shell goes through Session Manager |
| IAM instance profile | `startorch-instance` | S3 access and the shell |
| Security group | `startorch-web` | |
| Tag | `project=startorch`, lowercase | The start and stop permission depends on it |

Leave the volume's IOPS and throughput at their defaults; higher values cost extra. Turn on termination
protection, since *Stop* and *Terminate* sit in the same menu.

### 8.4 Set up the instance

Connect through EC2 → the instance → Connect → Session Manager, signed in as the console admin.

```bash
# 1. Update the system, then install Docker and the AWS CLI.
sudo apt update && sudo apt upgrade -y
curl -fsSL https://get.docker.com | sudo sh        # Docker Engine, with the Compose and Buildx plugins
sudo snap install aws-cli --classic
aws sts get-caller-identity                        # must show assumed-role/startorch-instance/...

# 2. Download the index: free, and about 10 to 15 minutes.
sudo mkdir -p /srv/index/posting/full-en
sudo aws s3 sync s3://<bucket>/lexical/full-en/2026-09-19/ /srv/index/posting/full-en/

# 3. Get the code, and write this machine's .env.
sudo git clone https://github.com/<you>/startorch.git /opt/startorch
sudo tee /opt/startorch/.env <<'EOF'
STARTORCH_PROFILE=full-en
INDEX_ROOT=/srv/index
SITE_ADDRESS=startorch.halzyonnn.dev
EOF

# 4. Build and start. The C++ build takes several minutes on 2 vCPUs.
cd /opt/startorch
sudo docker compose up -d --build
sudo docker compose logs -f        # wait for the index load and "certificate obtained successfully"
```

- **`INDEX_ROOT` is the folder that contains `posting/`.** The container sees `/srv/index` as
  `/data/scholar_rank`, so the code finds `posting/full-en/posting/metadata.bin` and
  `posting/full-en/lookup/doc_id_lookup.bin` where `project-config.toml` says they are.
- **Check the DNS name first.** It must resolve to the Elastic IP before Caddy starts with the domain.
- **If Session Manager says "Not connected"** with `send request failed`, the agent cannot reach AWS. This is a
  network error, not a permission error. Check that the security group allows all outbound traffic, then stop and
  start the instance.

### 8.5 Check it

From your own machine:

```bash
curl -I http://startorch.halzyonnn.dev/                 # 308, a redirect to HTTPS
curl https://startorch.halzyonnn.dev/api/readyz         # {"status":"ready","profile":"full-en"}
curl -I https://startorch.halzyonnn.dev/TESTING.md      # 404
```

Then open the site, and run the checklist in `frontend/TESTING.md` against it. Last, stop the instance, start it
again, and check `/api/readyz`. The stack must come back with no commands typed.

### 8.6 The stop alarm

A CloudWatch alarm on the instance's `CPUUtilization`, with the built-in action *Stop this instance*:

| Setting | Value |
|---|---|
| Statistic and period | Average, 5 minutes |
| Condition | `<= 2` |
| Datapoints to alarm | 12 out of 12, which is 1 hour |
| Missing data | Treated as good (not breaching) |
| EC2 action trigger | *In alarm*, not *OK* |

- **It is a timer, not an idle detector.** A search costs 30 to 50 ms of CPU, so light use looks the same as no
  use. The instance stops about 1 hour after it starts, unless something heavy runs.
- **It bounds a forgotten instance** at about $0.11. A demo that runs longer is cut off; start the instance again.
- **Why these settings.** An alarm acts only when its state changes. The boot and the index load push one
  5-minute period above 2%, which resets the alarm at each start. A stopped instance reports no data, and treating
  that as good returns the alarm to OK.

## Step 9. Protection

- In place: the input limits (step 1) and the search limit (step 2).
- If abuse appears, put Cloudflare's free tier in front: DNS, caching, rate limiting, and a hidden origin address.

## Step 10. Operations

**Run a demo.**

```bash
aws ec2 start-instances --instance-ids <instance-id> --profile startorch
aws ec2 stop-instances  --instance-ids <instance-id> --profile startorch
```

The site is ready 1 to 2 minutes after the start: the boot, then the index load from a cold disk. The first
searches are slower, until the posting pages they touch are cached.

**Stop, never terminate, between demos.**

| | Stop | Terminate |
|---|---|---|
| The instance | Switched off; it can start again | Deleted for good |
| The disk: index, image, `.env`, certificates | Kept | Deleted |
| Hourly instance charge | Stops | Stops |
| Disk charge | Continues | Stops |

Do not run `docker compose down` or `docker compose stop` before stopping the instance. Docker then leaves the
containers off at the next start. The fix is `sudo docker compose up -d` in `/opt/startorch`.

**Deploy a change.** On the instance: `cd /opt/startorch && sudo git pull && sudo docker compose up -d --build`.
Compose recreates only what changed. The index reload costs about a minute of downtime.

**Logs.** `sudo docker compose logs -f startorch`.

**Cost.** The budget is about $30 a month (us-east-2 on-demand prices).

| Item | Monthly cost | Charged while stopped |
|---|---|---|
| EBS gp3, one 80 GB volume | about $6.40 | yes |
| Public IPv4 (the Elastic IP) | about $3.65 | yes |
| S3, 52 GiB | about $1.30 | yes |
| **Fixed total** | **about $11.35** | |
| `r6a.large`, $0.113 per hour | about $18 for **160 hours** | no |

The instance is the only cost that grows with use. A month left running costs about $83, which is why the stop
alarm and an AWS Budgets alarm at $30 exist.

**Tear it down.** Terminate the instance, release the Elastic IP, and delete the alarm. The Elastic IP is billed
until it is released. The bucket and the DNS record stay until you remove them.

---

## Anti-patterns

- Memory-mapping posting files from S3.
- Several worker processes: each would load its own 6.7 GiB.
- Autoscaling: a new instance needs 52 GiB and tens of seconds before it can answer.
- Publishing the app's port instead of going through Caddy.
- Putting the index, `token_stream/`, or `posting/partial/` in the image.
- Putting access keys on the instance. Its role already gives it credentials.

## Decisions

1. Release the GIL and search in parallel (step 2).
2. Resolve ids in the browser, with no metadata store (step 3).
3. A static page with no framework. Cards show titles, with no abstracts (step 4).
4. xpac and deleted works stay in the index for now; PageRank should push them down (step 4).
5. Compose runs the app and Caddy. Caddy serves the page and handles HTTPS (step 7).
6. Everything that differs between machines is in `.env`, so one `compose.yaml` and one Caddyfile fit all (step 7).
7. The page is also in the image, for plain `docker run` (step 5).
8. Demo-only: the instance runs only when needed, for about $30 a month in total (step 10).
9. One disk, with no separate data volume and no snapshot. S3 is the backup, and a new download takes minutes
   (step 8).
10. The shell goes through Session Manager: no SSH key and no open port 22 (step 8).

---

## Open issues

### Skipped for now

| Item | Why it is skipped | Revisit when |
|---|---|---|
| Result cache: `functools.lru_cache(maxsize=1024)` on `(query, k)` | The load is small | Traffic repeats noticeably. Put it on a module-level function, normalize the query first, and return an immutable result. |
| OpenAlex rate limits | Keyless: 1,000 calls per day per visitor IP, and a search costs one call | Visitors hit the limit. A key must stay on the server, which means moving the lookups into the API. |
| CMake option to skip the GoogleTest fetch | It only costs image build time | Builds are automated, for example in CI. |
| Non-root `USER` in the image | The index is mounted read-only | Hardening matters more. Run `INSTALL fts` after `USER`. |
| Analysis packages (pandas, matplotlib, ipykernel, tabulate) in a dependency group | Not worth the complexity; the image is about 920 MB | Image size or pull time matters. |

### Not done, or known problems

- **The site is unreachable while the instance is stopped.** Visitors get a browser error, not the page's
  "demo may be offline" message. A fix: host the page on free static hosting (Cloudflare Pages, GitHub Pages) at
  `startorch.halzyonnn.dev`, and the API on EC2 at a second name such as `api.startorch.halzyonnn.dev`. The
  page then always loads and shows its offline message. The cost is CORS on the API and a configurable API URL in
  `app.js`.
- **Not confirmed: the $30 budget alarm, and the bucket's lifecycle rule.** Both are described above. Check that
  they exist.
- **The stop alarm cannot see light use.** It stops the instance about 1 hour after the start, even during a
  demo. A real idle check needs a script on the instance that reads the time of the last `/api/search` request.
- **Request timeout (step 9).** A pathological query can hold one of the 4 search slots. A timeout in Python only
  stops waiting: the C++ search keeps running on its thread. A real fix needs a time or work budget inside the
  engine.
- **Result quality.** Many full-en queries return mostly xpac records (datasets, "other"), whose short titles BM25
  favors. The example queries were picked to avoid this. Left to PageRank.
- **Deleted works.** Ids deleted from OpenAlex since the June 2026 snapshot show as muted cards. The next
  snapshot's `works/deleted_ids.csv` could filter them without a re-fetch.
- **OpenAlex dependency.** Titles need OpenAlex to be up. Without it, cards show ids only.
- **Memory.** full-en needs a 16 GiB machine. Options to shrink it: load only a term dictionary and read each
  query term's blocks on demand, or store document lengths more compactly.
- **Downtime on restart.** Every deploy or index refresh costs about a minute while the index reloads.
- **Single point of failure.** One process on one instance.
- **Ways to stretch the budget, if 160 hours is not enough.**
  - Spot instances: often 50 to 70% cheaper, but AWS can stop one with 2 minutes' notice, even mid-demo.
  - Graviton (`r6g.large`, `r7g.large`): about 10% cheaper than `r6a.large`, but the image and the index files
    must be checked on ARM.
  - Drop the Elastic IP (about $3.65 a month) by updating the DNS record on every start through the registrar's
    API.
  - Reduce query-engine memory (above) to fit an 8 GiB type, at about half the hourly price.

### Left out on purpose

| Left out | Add it when |
|---|---|
| Metadata store (titles, authors) | Titles must load without a second request, or offline |
| Image registry (ECR, GHCR) | Builds move off the instance, or a second machine pulls the image |
| Terraform, Packer | The environment is rebuilt more than a couple of times |
| Load balancer, ACM, Route 53 | More than one instance serves one name |
| WAF, CloudFront | Real traffic or real abuse; try Cloudflare's free tier first |
| OpenTelemetry, CloudWatch agent | Slowness needs debugging that logs cannot explain |
| CI/CD deploys | More than one person deploys |
| Blue/green index refresh | The restart downtime matters to someone |
| Multiple workers, autoscaling | One instance really saturates |
