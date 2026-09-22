"""The API's lifecycle with a fake searcher: background loading, load failure,
the concurrency limit, and a responsive event loop during slow searches.

The fake sleeps for a fixed time instead of loading an index, so the timing
assertions do not depend on the machine. Needs no index.
"""

import time
from datetime import timedelta
from types import SimpleNamespace

import anyio
import httpx
import pytest
from fastapi.testclient import TestClient

from startorch.api import api
from startorch.lexical.search import SearchResult


def fake_searcher(load_seconds: float = 0.0, search_seconds: float = 0.0):
    """Returns a Searcher stand-in class with fixed load and search times."""

    class FakeSearcher:
        instances = []

        def __init__(self, profile):
            time.sleep(load_seconds)
            self.profile = SimpleNamespace(kind="openalex")
            self.closed = False
            FakeSearcher.instances.append(self)

        def search(self, query, k):
            time.sleep(search_seconds)
            return SearchResult(hits=[(2626778328, 1.5)], elapsed=timedelta(milliseconds=2))

        def close(self):
            self.closed = True

    return FakeSearcher


@pytest.fixture(autouse=True)
def clean_resources():
    """Every test starts and ends with no app state."""
    api.resources.clear()
    yield
    api.resources.clear()


def poll(client, path: str, status: int, timeout: float = 10.0):
    """Polls path until it answers with status, and returns that response."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        r = client.get(path)
        if r.status_code == status:
            return r
        time.sleep(0.02)
    raise AssertionError(f"{path} never returned {status}")


def test_before_startup_only_healthz_succeeds():
    c = TestClient(api.app)   # no `with`: the lifespan never runs
    assert c.get("/healthz").status_code == 200
    assert c.get("/readyz").status_code == 503
    assert c.get("/search", params={"query": "x"}).status_code == 503


def test_serves_healthz_while_the_index_loads(monkeypatch):
    monkeypatch.setattr(api, "Searcher", fake_searcher(load_seconds=1.0))
    with TestClient(api.app) as c:
        assert c.get("/healthz").status_code == 200
        r = c.get("/readyz")
        assert r.status_code == 503 and "loading" in r.json()["detail"]
        assert c.get("/search", params={"query": "x"}).status_code == 503

        poll(c, "/readyz", 200)
        body = c.get("/search", params={"query": "x"}).json()
        assert body["hits"] == [{"id": "W2626778328", "score": 1.5}], "OpenAlex ids get their W prefix"


def test_failed_load_fails_both_health_checks(monkeypatch):
    monkeypatch.setattr(api, "PROFILE", "no-such-profile")
    with TestClient(api.app) as c:
        poll(c, "/healthz", 503)
        r = c.get("/readyz")
        assert r.status_code == 503 and "failed to load" in r.json()["detail"]


def test_shutdown_closes_the_searcher(monkeypatch):
    fake = fake_searcher()
    monkeypatch.setattr(api, "Searcher", fake)
    with TestClient(api.app) as c:
        poll(c, "/readyz", 200)
    assert fake.instances[0].closed
    assert api.resources == {}


@pytest.mark.index
def test_shutdown_releases_the_real_index():
    with TestClient(api.app) as c:
        poll(c, "/readyz", 200, timeout=60)
        searcher = api.resources["searcher"]
        c.get("/search", params={"query": "ocean"})   # creates a tokenizer cursor on a worker thread
        connections = [searcher.tokenizer.con, *searcher.tokenizer._cursors]   # close() empties the list
    assert api.resources == {}
    assert len(connections) > 1, "the search should have created a worker-thread cursor"
    for connection in connections:
        with pytest.raises(Exception):
            connection.execute("SELECT 1")


def test_shutdown_waits_for_a_load_in_progress(monkeypatch):
    fake = fake_searcher(load_seconds=0.5)
    monkeypatch.setattr(api, "Searcher", fake)
    with TestClient(api.app):
        pass                   # shut down at once, mid-load
    assert fake.instances and fake.instances[0].closed, "the finished load must still be released"


async def _with_app(fn):
    """Runs the app's lifespan around fn(client), with an async client."""
    async with api.lifespan(api.app):
        transport = httpx.ASGITransport(app=api.app)
        async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
            with anyio.fail_after(10):
                while (await client.get("/readyz")).status_code != 200:
                    await anyio.sleep(0.02)
            return await fn(client)


@pytest.mark.anyio
async def test_slow_search_does_not_block_healthz(monkeypatch):
    monkeypatch.setattr(api, "Searcher", fake_searcher(search_seconds=0.5))

    async def check(client):
        async with anyio.create_task_group() as tg:
            tg.start_soon(lambda: client.get("/search", params={"query": "x"}))
            await anyio.sleep(0.05)                    # the search is now running
            start = time.perf_counter()
            assert (await client.get("/healthz")).status_code == 200
            return time.perf_counter() - start

    healthz_seconds = await _with_app(check)
    assert healthz_seconds < 0.2, f"/healthz waited {healthz_seconds:.2f}s behind a search"


@pytest.mark.anyio
async def test_concurrent_searches_are_capped(monkeypatch):
    monkeypatch.setattr(api, "Searcher", fake_searcher(search_seconds=0.3))
    monkeypatch.setattr(api, "MAX_CONCURRENT_SEARCHES", 4)

    async def eight_at_once(client):
        start = time.perf_counter()
        async with anyio.create_task_group() as tg:
            for _ in range(8):
                tg.start_soon(lambda: client.get("/search", params={"query": "x"}))
        return time.perf_counter() - start

    elapsed = await _with_app(eight_at_once)
    # A limit of 4 runs 8 searches of 0.3 s in two waves; no limit would take one.
    assert 0.55 < elapsed < 2.0, f"8 searches took {elapsed:.2f}s"
