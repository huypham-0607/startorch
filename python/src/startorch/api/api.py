"""The HTTP API and search page: search over one profile's index, loaded in the background.

The API lives under /api. When the repository's frontend/ folder is present, the
search page is served at /, so `fastapi dev` runs the whole site on one port. In
production Caddy serves frontend/ itself and proxies only /api to this app.

The server accepts requests as soon as it starts. The index loads on a worker
thread, so /api/healthz answers throughout, and /api/readyz and /api/search
return 503 until the load finishes. Searches also run on worker threads, at most
MAX_CONCURRENT_SEARCHES at a time, so a slow search never blocks the event loop.
Both are possible because the C++ engine releases the GIL while it loads and
searches, and both it and the tokenizer are thread-safe.

The profile to serve comes from the STARTORCH_PROFILE environment variable,
msmarco by default.

Typical usage example, from python/:

    STARTORCH_PROFILE=full-en fastapi dev src/startorch/api/api.py
"""

import asyncio
import os
from contextlib import asynccontextmanager
from typing import Annotated

import anyio
from fastapi import APIRouter, FastAPI, HTTPException, Query
from fastapi.staticfiles import StaticFiles

from startorch.api.schemas import Hit, SearchResponse
from startorch.lexical.search import Searcher, SearchResult
from startorch.utils.logger import get_logger
from startorch.utils.misc import PROJECT_ROOT
from startorch.utils.paths import profile

logger = get_logger(__name__)

PROFILE = os.environ.get("STARTORCH_PROFILE", "msmarco")
FRONTEND_DIR = PROJECT_ROOT / "frontend"
MAX_K = 100
MAX_QUERY_LENGTH = 512
# Searches are CPU-bound, so more than about one per core only adds queueing.
MAX_CONCURRENT_SEARCHES = 4

resources = {}


async def _load_searcher() -> Searcher | None:
    """Loads the index on a worker thread and publishes it in resources.

    A failure is recorded in resources["load_error"] rather than raised, so the
    health endpoints can report it.

    Returns:
        The loaded Searcher, or None if the load failed.
    """
    try:
        searcher = await anyio.to_thread.run_sync(Searcher, profile(PROFILE))
    except Exception as e:
        logger.exception(f"Failed to load the index for profile {PROFILE}.")
        resources["load_error"] = e
        return None
    resources["searcher"] = searcher
    logger.info(f"Index for profile {PROFILE} is loaded; serving searches.")
    return searcher


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Starts loading the index in the background, and releases it on shutdown.

    Shutdown waits for a load still in progress, since a C++ load cannot be
    interrupted, then closes exactly the searcher this lifespan loaded.

    Args:
        app: The application being started.
    """
    resources["search_limiter"] = anyio.CapacityLimiter(MAX_CONCURRENT_SEARCHES)
    # This local reference also keeps the task from being garbage-collected
    # mid-load: asyncio itself holds tasks only weakly.
    loading = asyncio.create_task(_load_searcher())
    yield
    searcher = await loading
    if searcher is not None:
        searcher.close()
    resources.clear()


def _searcher() -> Searcher:
    """Returns the loaded Searcher.

    Raises:
        HTTPException: 503 if the index is still loading, or failed to load.
    """
    searcher = resources.get("searcher")
    if searcher is not None:
        return searcher
    if "load_error" in resources:
        raise HTTPException(status_code=503, detail=f"Index failed to load: {resources['load_error']}")
    raise HTTPException(status_code=503, detail="Index is still loading.")


def to_response(query: str, k: int, result: SearchResult, kind: str) -> SearchResponse:
    """Converts a Searcher result into the API's response shape.

    Args:
        query: The query as received.
        k: The number of results requested.
        result: What Searcher.search returned.
        kind: The profile kind. OpenAlex ids get their "W" prefix back; MS MARCO
            pids are left as they are.

    Returns:
        The response, with string ids and the search time in milliseconds.
    """
    prefix = "W" if kind == "openalex" else ""
    return SearchResponse(
        query=query,
        k=k,
        corpus="openalex" if kind == "openalex" else "msmarco",
        took_ms=result.elapsed.total_seconds() * 1000,
        hits=[Hit(id=f"{prefix}{raw_id}", score=score) for raw_id, score in result.hits],
    )


router = APIRouter(prefix="/api")


@router.get("/healthz")
async def healthz():
    """Liveness check: the process is running and can still become ready.

    Answers while the index loads, and never touches the index. Fails only if the
    load failed, which only a restart can fix.

    Raises:
        HTTPException: 503 if the index failed to load.
    """
    if "load_error" in resources:
        raise HTTPException(status_code=503, detail="Index failed to load; restart required.")
    return {"status": "ok"}


@router.get("/readyz")
async def readyz():
    """Readiness check: the index is loaded and searches can be served.

    Returns:
        The profile being served.

    Raises:
        HTTPException: 503 if the index is still loading, or failed to load.
    """
    _searcher()
    return {"status": "ready", "profile": PROFILE}


@router.get("/search")
async def search(
    query: Annotated[str, Query(min_length=1, max_length=MAX_QUERY_LENGTH)],
    k: Annotated[int, Query(ge=1, le=MAX_K)] = 10,
) -> SearchResponse:
    """Runs a BM25 top-k search over the loaded index, on a worker thread.

    A query of only stopwords or unknown words returns an empty hit list.

    Args:
        query: Free-text query, 1 to MAX_QUERY_LENGTH characters.
        k: Number of results, 1 to MAX_K.

    Returns:
        Up to k hits, best first.

    Raises:
        HTTPException: 503 if the index is still loading, or failed to load.
    """
    searcher = _searcher()
    result = await anyio.to_thread.run_sync(searcher.search, query, k, limiter=resources["search_limiter"])
    return to_response(query, k, result, searcher.profile.kind)


app = FastAPI(lifespan=lifespan)
app.include_router(router)

# Mounted last, so /api routes match first. The folder is absent where only the
# API is deployed (Caddy serves the page there), so the mount is optional.
if FRONTEND_DIR.is_dir():
    app.mount("/", StaticFiles(directory=FRONTEND_DIR, html=True), name="frontend")
