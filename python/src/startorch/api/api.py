"""The HTTP API: search over one profile's index, loaded once at startup.

Every endpoint is `async def`, so every request runs on the event loop's one
thread. That keeps searches from overlapping, which matters because the
tokenizer's DuckDB connection is not thread-safe. The cost is that a slow search
delays every request queued behind it, /healthz included.

Typical usage example, from python/:

    fastapi dev src/startorch/api/api.py
"""

from contextlib import asynccontextmanager
from typing import Annotated

from fastapi import FastAPI, HTTPException, Query

from startorch.api.schemas import Hit, SearchResponse
from startorch.lexical.search import Searcher, SearchResult
from startorch.utils.paths import profile

PROFILE = "msmarco"
MAX_K = 100
MAX_QUERY_LENGTH = 512

resources = {}


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Loads the index before the server accepts requests, and releases it on shutdown.

    Loading happens before `yield`, so Uvicorn starts serving only once the
    index is ready. Loading on a background thread would not help yet: the C++
    constructor holds the GIL, so the event loop would freeze for the whole load.

    Args:
        app: The application being started.
    """
    resources["searcher"] = Searcher(profile(PROFILE))
    yield
    resources.pop("searcher").close()


app = FastAPI(lifespan=lifespan)


def _searcher() -> Searcher:
    """Returns the loaded Searcher.

    Raises:
        HTTPException: 503 if the index is not loaded yet.
    """
    searcher = resources.get("searcher")
    if searcher is None:
        raise HTTPException(status_code=503, detail="Index is still loading.")
    return searcher


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
        took_ms=result.elapsed.total_seconds() * 1000,
        hits=[Hit(id=f"{prefix}{raw_id}", score=score) for raw_id, score in result.hits],
    )


@app.get("/")
async def root():
    """Returns a greeting, confirming the app is up."""
    return {"message": "Hello World"}


@app.get("/healthz")
async def healthz():
    """Liveness check: the process is running and the event loop responds.

    Does no work, and never touches the index.
    """
    return {"status": "ok"}


@app.get("/readyz")
async def readyz():
    """Readiness check: the index is loaded and searches can be served.

    Returns:
        The profile being served.

    Raises:
        HTTPException: 503 if the index is not loaded yet.
    """
    _searcher()
    return {"status": "ready", "profile": PROFILE}


@app.get("/search")
async def search(
    query: Annotated[str, Query(min_length=1, max_length=MAX_QUERY_LENGTH)],
    k: Annotated[int, Query(ge=1, le=MAX_K)] = 10,
) -> SearchResponse:
    """Runs a BM25 top-k search over the loaded index.

    A query of only stopwords or unknown words returns an empty hit list.

    Args:
        query: Free-text query, 1 to MAX_QUERY_LENGTH characters.
        k: Number of results, 1 to MAX_K.

    Returns:
        Up to k hits, best first.

    Raises:
        HTTPException: 503 if the index is not loaded yet.
    """
    searcher = _searcher()
    result = searcher.search(query, k)
    return to_response(query, k, result, searcher.profile.kind)
