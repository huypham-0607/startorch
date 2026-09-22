"""The HTTP API against the real msmarco index: search results, validation, concurrency.

Lifecycle behavior (loading, load failure, the concurrency limit) is tested with
a fake searcher in test_api_lifecycle.py.
"""

import time

import anyio
import httpx
import pytest
from fastapi.testclient import TestClient

from startorch.api import api

pytestmark = pytest.mark.index


def wait_ready(client: TestClient, timeout: float = 60.0) -> None:
    """Polls /readyz until the background load finishes."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if client.get("/readyz").status_code == 200:
            return
        time.sleep(0.02)
    raise AssertionError("the index never became ready")


@pytest.fixture(scope="module")
def client():
    """A client whose app has loaded the msmarco index, shared by this module."""
    with TestClient(api.app) as c:   # runs the lifespan: startup, then shutdown at exit
        wait_ready(c)
        yield c


def test_health_endpoints_once_ready(client):
    assert client.get("/healthz").json() == {"status": "ok"}
    assert client.get("/readyz").json() == {"status": "ready", "profile": "msmarco"}


def test_search_response_shape(client):
    r = client.get("/search", params={"query": "what is the bm25 ranking function", "k": 5})
    assert r.status_code == 200
    body = r.json()
    assert body["query"] == "what is the bm25 ranking function" and body["k"] == 5
    assert isinstance(body["took_ms"], float) and body["took_ms"] > 0
    assert len(body["hits"]) == 5
    assert all(isinstance(h["id"], str) and not h["id"].startswith("W") for h in body["hits"]), "MS MARCO ids stay bare"
    scores = [h["score"] for h in body["hits"]]
    assert scores == sorted(scores, reverse=True)


def test_k_defaults_to_10(client):
    assert len(client.get("/search", params={"query": "ocean temperature"}).json()["hits"]) == 10


@pytest.mark.parametrize("params", [
    {"query": "x", "k": 0},
    {"query": "x", "k": api.MAX_K + 1},
    {"query": "x", "k": "ten"},
    {"k": 5},
    {"query": ""},
    {"query": "a" * (api.MAX_QUERY_LENGTH + 1)},
], ids=["k=0", "k>max", "k not int", "no query", "empty query", "query too long"])
def test_invalid_input_is_rejected(client, params):
    assert client.get("/search", params=params).status_code == 422


def test_stopword_only_query_returns_no_hits(client):
    r = client.get("/search", params={"query": "the and of"})
    assert r.status_code == 200 and r.json()["hits"] == []


def test_trailing_slash_redirects_to_search(client):
    r = client.get("/search/", params={"query": "x"}, follow_redirects=False)
    assert r.status_code == 307
    assert r.headers["location"].split("?")[0].endswith("/search")


def test_openapi_documents_the_response_model(client):
    assert "SearchResponse" in client.get("/openapi.json").json()["components"]["schemas"]


def test_concurrent_searches_match_sequential(client, msmarco_queries):
    """Many requests at once, run on worker threads, must return the sequential results."""
    queries = msmarco_queries[:60]
    expected = [client.get("/search", params={"query": q, "k": 20}).json()["hits"] for q in queries]

    async def fire_all():
        transport = httpx.ASGITransport(app=api.app)   # the app is already loaded by the fixture
        async with httpx.AsyncClient(transport=transport, base_url="http://test") as ac:
            got = [None] * len(queries)

            async def one(i):
                got[i] = (await ac.get("/search", params={"query": queries[i], "k": 20})).json()["hits"]

            async with anyio.create_task_group() as tg:
                for i in range(len(queries)):
                    tg.start_soon(one, i)
            return got

    assert anyio.run(fire_all) == expected
