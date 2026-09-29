# Deploying Startorch as a live search service (EC2 + S3)

The plan for putting the query engine behind a public URL, sized for **a small number of users**: a portfolio
demo, a few friends, an occasional reviewer. Two rules shape it: **write as little as possible**, and **depend on
as little as possible**.

Last updated 2026-09-27. Future work and known problems are in [Open issues](#open-issues).

---

## Status

**The whole site runs locally in Docker Compose, on full-en, behind Caddy. Nothing is on AWS yet.**

| Step | Status |
|---|---|
| 1. FastAPI wrapper | Done |
| 2. Concurrency | Done |
| 3. Results without a metadata store | Done |
| 4. Front end | Done |
| 5. Docker image | Done |
| 6. Index to S3 and the instance | Not started |
| 7. Compose + Caddy | Done locally. Not on EC2 yet. |
| 8. Protection | Input limits and the search limit are in. |
| 9. Operations | Not started |

**Next.** Deploy to EC2 at `startorch.halzyonnn.dev`, following the
[EC2 walkthrough](#ec2-walkthrough-startorchhalzyonnndev). The domain is bought.

**Run it locally.**
- In Docker: from the repository root, `docker compose up -d --build`, then open `http://localhost/`.
- Without Docker: from `python/`, `STARTORCH_PROFILE=full-en fastapi dev src/startorch/api/api.py`, then open
  `http://127.0.0.1:8000/`. Without the variable, the profile is `msmarco`, whose results have no titles.

**Tests.** `uv run pytest` (94 tests, including the API and the served page), `node --test frontend/tests/`,
`ctest` (198 tests), and the manual browser checklist in `frontend/TESTING.md`.

### Measurements (full-en)

| Quantity | Value |
|---|---|
| Files needed to serve | About 56 GB: posting files 47.7, id lookup 4.15, block metadata 2.25, document lengths 1.73 |
| Memory | 6.7 GiB after the load (7.0 GiB in the container), 8.7 GiB peak while loading |
| Load time | 14 to 16 s with a warm cache |
| Search time | 30 to 50 ms for the page's example queries; up to about 600 ms for common words |
| Parallel search speedup | 1.7× on 2 threads, 2.7× on 4, 3.6× on 8 |
| Docker image | About 920 MB |
| msmarco, for comparison | 0.4 s load, 0.3 GiB |

**Target machine:** one EC2 instance with 16 GiB of RAM (`r6a.large`), a 20 GB root volume, and a 64 GB gp3 data
volume, run only for demos.

## The stack

| Concern | Choice |
|---|---|
| HTTP API | FastAPI + Uvicorn, one process |
| Page | Static HTML, plain JavaScript, Pico.css from a CDN |
| Titles and metadata | The OpenAlex API, called from the browser |
| Build and run | Docker multi-stage build, Docker Compose |
| Page, proxy, TLS | Caddy, as the second Compose service |
| Index storage | S3, copied to an EBS volume with the AWS CLI |
| Machine | One EC2 instance, reached through SSM Session Manager |
| Supervision | Docker's restart policy, with Docker waiting for the index volume |
| Cost control | An AWS Budgets alarm, and a CloudWatch alarm that stops the idle instance |

---

## EC2 walkthrough: startorch.halzyonnn.dev

Steps 6 and 7 done for real: the stack from step 7 on one EC2 instance, over HTTPS at
`https://startorch.halzyonnn.dev`. To be pruned into the steps once it works.

**Two facts to know before starting.**
- **`.dev` is HTTPS-only.** Every `.dev` domain is on the browsers' HSTS preload list, so browsers refuse plain
  HTTP for it. The site works in a browser only once Caddy has its certificate. To debug before that, run Caddy on
  `:80` and use the Elastic IP.
- **DNS is at Porkbun, and the name is parked.** Right now `startorch.halzyonnn.dev` resolves to Porkbun's
  parking page (`pixie.porkbun.com`), through its default wildcard record. An A record for `startorch` overrides it.

**Budget: demo-only, about $30 a month.** The instance runs only when someone is looking at it. Everything else
is charged all month, running or not, so it is kept small (us-east-1 on-demand prices; check your region):

| Item | Monthly cost | Charged while stopped |
|---|---|---|
| EBS gp3: 20 GB root + 64 GB data | about $6.70 | yes |
| Public IPv4 (the Elastic IP) | about $3.65 | yes |
| S3, 56 GB (the index backup) | about $1.30 | yes |
| **Fixed total** | **about $11.70** | |
| `r6a.large` (2 vCPU, 16 GiB), $0.113 per hour | about $18 for **160 hours** | no |

- **160 hours** is about 5 hours a day, or 40 a week. The instance is the only cost that scales with use.
- **No EBS snapshot.** The S3 copy is the backup; a snapshot would add about $3 a month for the same thing.
- **Forgetting to stop the instance is the real risk:** a month left running costs about $83. An idle alarm
  stops it automatically (step 18).
- Data transfer is effectively free at this scale: the first 100 GB out per month are free, and the page's
  OpenAlex lookups go from the browser to OpenAlex, not through AWS.

### A. Prepare locally

1. **Set a budget alarm first.** Billing → Budgets → a monthly cost budget of $30, with email alerts at 50%, 80%
   and 100%, and one on the *forecasted* amount, which warns early when an instance was left running. New
   accounts may also have Free Tier credits; check Billing → Credits.
2. **Set up the AWS CLI with an IAM user or IAM Identity Center, never root keys.** Run `aws configure`, and pick
   one region for everything. The bucket and the instance must be in the same region, so the S3 download is free.
3. **Make the site address a variable,** so one Caddyfile serves `:80` locally and the domain on EC2. Caddy reads
   `{$NAME:default}` from its environment.
   - In `Caddyfile`, change the first line to:
     ```
     {$SITE_ADDRESS::80} {
     ```
   - In `compose.yaml`, on the `caddy` service:
     ```yaml
     environment:
         SITE_ADDRESS: ${SITE_ADDRESS:-:80}   # never empty: an empty address breaks the Caddyfile
     ports:
         - "80:80"
         - "443:443"
         - "443:443/udp"                     # HTTP/3
     ```
   - Check locally: `docker compose up -d --force-recreate caddy` still serves `http://localhost/`.
   - Commit and push. The instance will `git clone` the repository. If it's private, create a read-only GitHub
     fine-grained token or a deploy key for the instance.
4. **Create the bucket and upload the index.** Keep the defaults: block all public access, encryption on. Turn
   on versioning.
   ```
   aws s3 mb s3://<bucket> --region <region>
   aws s3api put-bucket-versioning --bucket <bucket> --versioning-configuration Status=Enabled
   aws s3 sync /data/scholar_rank/posting/full-en s3://<bucket>/indexes/full-en/2026-09-19/ \
       --exclude "token_stream/*" --exclude "posting/partial/*"
   ```
   The upload is 56 GB, so its time depends on your upload speed: about 2.5 hours at 50 Mbit/s. If it stops,
   run the same command again; `sync` skips the files already uploaded.

### B. Create the AWS resources

5. **An IAM role for the instance.** IAM → Roles → a role trusted by EC2, with:
   - `AmazonSSMManagedInstanceCore`, for shell access through Session Manager;
   - read access to the index prefix only: `s3:ListBucket` on the bucket, and `s3:GetObject` on
     `arn:aws:s3:::<bucket>/indexes/*`.

   The instance then gets short-lived credentials from its role, with no keys stored on it.
6. **A security group.** Inbound: TCP 80 and TCP 443 from anywhere, plus UDP 443 for HTTP/3. Nothing else: no port
   22, since the shell goes through Session Manager. Outbound: the default, allow all.
7. **Launch the instance.**
   - AMI: Ubuntu Server 24.04 LTS, x86_64. It has the SSM agent, and Docker's own packages include Compose and
     Buildx. Stay on x86_64: the index files were built and tested there.
   - Type: `r6a.large` (2 vCPU, 16 GiB, AMD), the cheapest x86_64 type with 16 GiB. 16 GiB is the floor: the
     load peaks at 8.7 GiB, and 8 GiB types cannot hold it.
   - Storage: a 20 GB gp3 root volume (OS, Docker, images, build cache), plus a 64 GB gp3 data volume for the
     53 GiB index. With a separate volume, the index survives replacing the instance.
   - The IAM role and security group from steps 5 and 6. No key pair.
8. **Allocate an Elastic IP,** and associate it with the instance.
9. **Point the name at it.** In Porkbun → Domain Management → DNS, add an `A` record: host `startorch`, answer the
   Elastic IP, TTL 600. The specific record overrides the parking wildcard. Check it from your machine:
   `getent hosts startorch.halzyonnn.dev` must print the Elastic IP before step 15.

### C. Set up the instance

Connect through EC2 → the instance → Connect → Session Manager. From your terminal instead, you need the
Session Manager plugin, then `aws ssm start-session --target <instance-id>`.

10. **Install Docker and the AWS CLI.**
    ```
    curl -fsSL https://get.docker.com | sudo sh     # Docker Engine, the Compose and Buildx plugins
    sudo snap install aws-cli --classic
    ```
11. **Format and mount the data volume.** Use a label, since NVMe device names can change between boots:
    ```
    lsblk                                           # the 64 GB disk, e.g. /dev/nvme1n1
    sudo mkfs.ext4 -L startorch-index /dev/nvme1n1
    sudo mkdir -p /srv/index
    echo 'LABEL=startorch-index /srv/index ext4 defaults,nofail 0 2' | sudo tee -a /etc/fstab
    sudo mount -a && df -h /srv/index
    ```
12. **Download the index** into the layout that `project-config.toml` expects under `INDEX_ROOT`:
    ```
    sudo aws s3 sync s3://<bucket>/indexes/full-en/2026-09-19/ /srv/index/posting/full-en/
    ```
13. **Make Docker wait for the index.** `restart: unless-stopped` already brings the stack back after a reboot,
    since Docker starts at boot. But without this, Docker could start before the volume is mounted, and the app
    would find an empty folder:
    ```
    sudo systemctl edit docker        # add the two lines below, then save
    [Unit]
    RequiresMountsFor=/srv/index
    ```
    This replaces the separate systemd unit planned in step 7.
14. **Clone the repository, and write `.env`.**
    ```
    sudo git clone https://github.com/<you>/startorch.git /opt/startorch
    sudo tee /opt/startorch/.env <<'EOF'
    STARTORCH_PROFILE=full-en
    INDEX_ROOT=/srv/index
    SITE_ADDRESS=startorch.halzyonnn.dev
    EOF
    ```
15. **Start it.** Check again that the DNS name resolves to the Elastic IP first. Let's Encrypt allows only 5
    failed validations per hostname per hour.
    ```
    cd /opt/startorch
    sudo docker compose up -d --build     # the C++ build takes several minutes on 2 vCPUs
    sudo docker compose logs -f           # wait for the index load and "certificate obtained successfully"
    ```
    The first index load reads about 4 GB from a fresh gp3 volume, at 125 MiB/s, so expect it to take longer than
    locally.

### D. Check it, and make it last

16. **Check from your machine.**
    ```
    curl -I http://startorch.halzyonnn.dev/                 # 308, redirect to HTTPS
    curl https://startorch.halzyonnn.dev/api/readyz          # {"status":"ready","profile":"full-en"}
    curl -I https://startorch.halzyonnn.dev/TESTING.md       # 404
    ```
    Then open the site, and run the checklist in `frontend/TESTING.md` against it.
17. **Stop and start test.** Stop the instance, start it again, and check `/api/readyz`. The stack should come
    back with no commands typed. This is the cycle every demo goes through.
18. **Stop it when idle, automatically.** CloudWatch → Alarms → create an alarm on the instance's
    `CPUUtilization`: average below 2% for 6 consecutive 10-minute periods. Its action: *Stop this instance*.
    This is a built-in alarm action, so there is no script. An idle server with a loaded index uses almost no
    CPU, so after an hour with no searches, the instance stops itself.
19. **Skip the uptime monitor for now.** With the instance stopped most of the time, it would alert constantly.

**Running a demo.**
```
aws ec2 start-instances --instance-ids <instance-id>     # or Start in the console
```
The site is ready 1 to 2 minutes later: the boot, then the index load from a cold disk. The first searches are
slower, until the posting pages they touch are cached. Stop it afterwards with
`aws ec2 stop-instances --instance-ids <instance-id>`, or let the idle alarm do it.

While it's stopped, the Elastic IP, the DNS record, the volumes and Caddy's certificates all survive. Visitors
get a browser connection error, not a page (see Open issues).

**Deploying a change later.**
```
cd /opt/startorch && sudo git pull && sudo docker compose up -d --build
```
Compose recreates only the containers whose image or config changed. After editing the Caddyfile, run
`sudo docker compose up -d --force-recreate caddy`.

**Research.** IAM roles and instance profiles, S3 bucket policies, Session Manager, EBS volumes and `/etc/fstab`,
systemd drop-ins and `RequiresMountsFor`, Caddy environment placeholders, HSTS preload, Let's Encrypt rate limits,
CloudWatch alarm actions, AWS Budgets forecast alerts.

---

## Step 1. FastAPI wrapper — done

`python/src/startorch/api/`:
- The lifespan handler loads the index once, as a background task, so the server answers at once.
- Routes, all under `/api`, so `/` is free for the page:
  - `/api/search?query=&k=` returns `{query, k, corpus, took_ms, hits: [{id, score}]}`. `k` must be 1 to 100,
    and the query 1 to 512 characters. Anything else returns 422.
  - `/api/healthz`: the process is alive. It answers during the load.
  - `/api/readyz`: the index is loaded. It and `/api/search` return 503 until then.
- A failed load makes both health checks return 503, since only a restart fixes it.
- `STARTORCH_PROFILE` picks the index, so one image serves any profile. The default is `msmarco`.

## Step 2. Concurrency — done

- The bindings release the GIL while loading and searching.
- The C++ query path is `const`, so the compiler rejects writes to shared state. ThreadSanitizer finds no data
  race with 8 threads sharing one engine.
- The tokenizer gives each thread its own DuckDB cursor. The shared connection was the real failure: 322 of 400
  threaded searches failed before this fix.
- Each search runs on a worker thread (`anyio.to_thread.run_sync`), at most 4 at a time (`CapacityLimiter`).
  Extra requests wait for a slot. A slow search never blocks the event loop.

## Step 3. Results without a metadata store — done

The index holds ids and scores only. The API returns OpenAlex ids with their `W` prefix, and the page resolves
them with one OpenAlex request per search:
`https://api.openalex.org/works?filter=ids.openalex:W1|W2|...&include_xpac=true`. MS MARCO ids stay bare.

## Step 4. Front end — done

`frontend/`: `index.html`, `app.js` (page logic), `format.js` (pure formatting, tested with Node), `style.css`.
- The page shows one card per hit at once, in rank order. One OpenAlex request then fills them: title, authors,
  year, venue, citation count, DOI and open-access links.
- `include_xpac=true` is required. Without it, OpenAlex hides its expansion records, about 42% of the corpus.
- A missing title shows `[Untitled]`. Works deleted since the snapshot get a muted card. If OpenAlex is down,
  the cards keep their rank and id.
- The query is in the URL (`/?q=`), so a search can be shared. While the index loads, the page waits, then
  searches by itself.
- Everything from the network goes in with `textContent`, never `innerHTML`.
- The page calls `/api/...` on its own origin, so it works unchanged behind FastAPI or Caddy.

## Step 5. Docker image — done

`Dockerfile` and `.dockerignore`, at the repository root. Two stages, both on `python:3.14-slim`:
- **Build.** A compiler, CMake, git, and uv. Then three layers, the least-changing first:
  1. the dependencies (`pyproject.toml`, `uv.lock`);
  2. the C++ module (the `startorch_cpp` target only);
  3. the Python source, with a second `uv sync` to install the package.
- **Runtime.** Copies `python/` (the venv, the source and the `.so`) and `project-config.toml`. No compiler. It
  also installs DuckDB's `fts` extension and copies `frontend/`, so plain `docker run` serves the whole site.
- `CMD` runs one Uvicorn process on `0.0.0.0:8000`.

The index is never in the image. It is bind-mounted read-only at `/data/scholar_rank`, where every path in
`project-config.toml` starts:

```
docker run -p 8000:8000 -e STARTORCH_PROFILE=full-en -v /data/scholar_rank:/data/scholar_rank:ro startorch
```

**Notes.**
- `.dockerignore` needs `**/*.so`. A bare `*.so` matches only the context root, so the host's `.so` would
  replace the one built in the image (`GLIBCXX_... not found`).
- `fts` must be installed at build time. Otherwise DuckDB downloads it on the first search, and searches fail
  without a network.
- Docker Desktop runs containers in a VM, and the VM's memory is the real limit; `-m` cannot go above it. Raise
  it under Settings → Resources. EC2 has no such VM.
- When a container dies, `docker inspect <name> --format '{{.State.ExitCode}} {{.State.OOMKilled}}'` tells an
  OOM kill (137, `true`) from a crash, and `docker logs <name>` shows its last output.
- Build for the architecture you deploy on. For Graviton, use `docker buildx --platform`, and re-check the index
  files, which use native byte order and struct sizes.

## Step 6. Index to S3, then to the instance

- Upload only the serving files, to a versioned prefix:
  ```
  aws s3 sync /data/scholar_rank/posting/full-en s3://<bucket>/indexes/full-en/2026-09-19/ \
      --exclude "token_stream/*" --exclude "posting/partial/*"
  ```
- On the instance, `aws s3 sync` it down into `<volume>/posting/full-en/` on a gp3 volume. Keep that layout:
  the container sees `<volume>` as `/data/scholar_rank` (step 7).
- Turn on S3 versioning. Snapshot the data volume once it is populated, so a rebuilt instance skips the download.
- **Never mount S3 as a file system** (Mountpoint, s3fs). The engine memory-maps posting files and reads them
  randomly, so every page fault would become a network round trip.

**Research.** `aws s3 sync`, S3 versioning, EBS gp3, EBS snapshots, VPC gateway endpoint for S3.

## Step 7. Compose + Caddy — done locally

`compose.yaml`, `Caddyfile`, and a gitignored `.env`, at the repository root.

**`compose.yaml`, two services.**
- `startorch`: the image's `runtime` stage.
  - The profile comes from `.env`: `STARTORCH_PROFILE: ${STARTORCH_PROFILE:-msmarco}`.
  - The index: `${INDEX_ROOT:-/data/scholar_rank}:/data/scholar_rank:ro`. Only the left side changes between
    machines.
  - `mem_limit` and `memswap_limit` are both 12g; `restart: unless-stopped`.
  - No published ports. Caddy reaches it at `startorch:8000`: on the Compose network, a service's name is its
    hostname.
- `caddy`: `caddy:2`, publishing port 80.
  - Read-only bind mounts: `./Caddyfile` at `/etc/caddy/Caddyfile`, and `./frontend` at `/srv/frontend`. `./` is
    relative to `compose.yaml`.
  - Named volumes: `caddy_data` at `/data` (certificates) and `caddy_config` at `/config`.

**`.env`, one per machine.** Compose uses it only to fill in `${...}`; the `environment:` key passes the profile
into the container. Use `KEY=value`:

```
STARTORCH_PROFILE=full-en
INDEX_ROOT=/data/scholar_rank
```

**Run it.**

```
docker compose up -d --build
docker compose logs -f startorch      # wait for "Index for profile full-en is loaded"
docker compose down                   # stop; add -v only to delete Caddy's certificates too
```

Checked locally on 2026-09-26, on full-en: ready in about 16 s, searches in 30 to 40 ms through Caddy, the test
files return 404, and `localhost:8000` is unreachable.

**The Caddyfile, before a domain.**

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

- `:80` is the site address: any hostname, port 80, plain HTTP.
- `@hidden` is a named matcher, a condition that directives can refer to.
- The `handle` blocks are mutually exclusive. A request runs only the first match, and the block with no matcher
  always goes last.

**Switching to a domain.**
1. Buy a domain, and add a DNS A record: `startorch.halzyonnn.dev` → the Elastic IP.
2. Make the Caddyfile's site address `{$SITE_ADDRESS::80}`, and pass `SITE_ADDRESS` to `caddy` from `.env`.
   The same file then serves `:80` locally and the domain on EC2 (walkthrough, step 3).
3. In `compose.yaml`, also publish `"443:443"` on `caddy`, and `"443:443/udp"` for HTTP/3.
4. Set `SITE_ADDRESS=startorch.halzyonnn.dev` in the instance's `.env`, and run
   `docker compose up -d --force-recreate caddy`.

Caddy then gets and renews a Let's Encrypt certificate by itself, and redirects HTTP to HTTPS. The page and the API
need no change.

**Notes when deploying.**
- **Caddy sorts directives** into a fixed order, whatever the order written. A bare `respond` beside `handle`
  blocks runs after them, so put it inside its own `handle`.
- **After editing the Caddyfile, recreate Caddy** (`docker compose up -d --force-recreate caddy`). A single-file
  bind mount follows the file's inode, and editors often save a new file, so `caddy reload` reloads the old one.
- **Keep `caddy_data`** once there is a domain. `docker compose down -v` deletes it, and each new certificate
  counts toward Let's Encrypt's limit of 5 per week.
- **Memory.** `mem_limit` must stay above the 8.7 GiB load peak, or the container dies mid-load with no traceback.
  Memory-mapped posting pages count toward the limit, but the kernel evicts them before killing anything.
  `memswap_limit` equal to `mem_limit` keeps the heap out of swap.
- **Elastic IP.** A plain EC2 public IP changes on every stop; an Elastic IP does not, so DNS never needs
  updating. Every public IPv4 address costs a small hourly fee.
- **Security group:** ports 80 and 443 only. Shell access goes through SSM Session Manager.
- **Supervision:** `restart: unless-stopped` restarts the stack at boot. A systemd drop-in makes Docker wait for
  the index volume (walkthrough, step 13).
- **Plain HTTP** shows as "Not secure" in browsers, so don't share the address widely before switching to a
  domain.

**Research.** Elastic IPs, DNS A records, systemd units that wrap Compose, cgroup v2 memory accounting.

## Step 8. Protection — partly done

- In place: the input limits (step 1) and the search limit (step 2).
- If abuse appears, put Cloudflare's free tier in front: DNS, caching, rate limiting, and a hidden origin address.

## Step 9. Operations

- **Logs:** `docker compose logs -f startorch`. Cap Docker's log files with the `logging:` key's `max-size`, or
  use the `journald` log driver.
- **Uptime:** skipped while the instance runs only for demos. If it ever runs all the time, add a free external
  monitor on `/api/healthz`.
- **Cost:** an AWS Budgets alarm, set before the first instance runs overnight. Stopping the instance when idle is
  the simplest saving; the Elastic IP and the EBS volume keep the setup intact.
- **Deploy:** `git pull`, then `docker compose up -d --build`. Compose recreates only what changed. About a
  minute of downtime while the index reloads.
- **Before sharing:** a load test (`hey` or `ab` against `/api/search`), then the checklist in
  `frontend/TESTING.md` against the public address.

---

## Anti-patterns

- Memory-mapping posting files from S3.
- Several worker processes: each would load its own 6.7 GiB.
- Autoscaling: a new instance needs 56 GB and tens of seconds before it can answer.
- Publishing the app's port instead of going through Caddy.
- Putting the index, `token_stream/`, or `posting/partial/` in the image.

## Decisions

1. Release the GIL and search in parallel (step 2).
2. Resolve ids in the browser, with no metadata store (step 3).
3. A static page with no framework. Cards show titles, with no abstracts (step 4).
4. xpac and deleted works stay in the index for now; PageRank should push them down (step 4).
5. Compose runs the app and Caddy. Caddy serves the page, so the page stays up when the app is down (step 7).
6. The profile and the index location come from `.env`, so one `compose.yaml` fits every machine (step 7).
7. The page is also in the image, for plain `docker run` (step 5).
8. Demo-only: the instance runs only when needed, for about $30 a month in total (walkthrough).

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

- **Request timeout (step 8).** A pathological query can hold one of the 4 search slots. A timeout in Python only
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
- **The site is unreachable while the instance is stopped.** Visitors get a browser error, not the page's
  "demo may be offline" message. A fix: host the page on free static hosting (Cloudflare Pages, GitHub Pages) at
  `startorch.halzyonnn.dev`, and the API on EC2 at a second name such as `api.startorch.halzyonnn.dev`. The
  page then always loads and shows its offline message. The cost is CORS on the API and a configurable API URL in
  `app.js`.
- **Ways to stretch the budget, if 160 hours is not enough.**
  - Spot instances: often 50 to 70% cheaper, but AWS can stop one with 2 minutes' notice, even mid-demo.
  - Graviton (`r6g.large`, `r7g.large`): about 10% cheaper than `r6a.large`, but the image and the index files
    must be checked on ARM.
  - Drop the Elastic IP (about $3.65 a month) by updating the DNS record on every start through Porkbun's API.
  - Reduce query-engine memory (above) to fit an 8 GiB type, at about half the hourly price.
- **HTTPS before a domain.** Possible with a wildcard DNS name such as sslip.io, but those names share Let's
  Encrypt's limits with every other user.

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
