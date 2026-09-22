"""The search page as the browser sees it: served files, content types, and the API it reads.

Needs no index. The page's own logic is tested with Node:
    node --test frontend/tests/
"""

from datetime import timedelta
from types import SimpleNamespace

import pytest
from fastapi.testclient import TestClient

from startorch.api import api
from startorch.lexical.search import SearchResult


@pytest.fixture(autouse=True)
def clean_resources():
    api.resources.clear()
    yield
    api.resources.clear()


@pytest.fixture
def site():
    return TestClient(api.app)   # no lifespan: static files and routing only


def test_frontend_folder_is_mounted():
    assert api.FRONTEND_DIR.is_dir(), f"{api.FRONTEND_DIR} is missing, so / has no page"


def test_page_is_served_at_root(site):
    r = site.get("/")
    assert r.status_code == 200
    assert r.headers["content-type"].startswith("text/html")
    assert 'id="search-form"' in r.text and 'src="app.js"' in r.text


@pytest.mark.parametrize("path", ["/app.js", "/format.js"])
def test_scripts_are_served_as_javascript(site, path):
    # Browsers refuse to run a type="module" script served with any other type.
    r = site.get(path)
    assert r.status_code == 200
    assert "javascript" in r.headers["content-type"]


def test_stylesheet_is_served_as_css(site):
    r = site.get("/style.css")
    assert r.status_code == 200 and r.headers["content-type"].startswith("text/css")


def test_unknown_paths_are_404(site):
    assert site.get("/api/no-such-route").status_code == 404
    assert site.get("/no-such-file.js").status_code == 404


def test_openalex_response_names_its_corpus_and_prefixes_ids():
    result = SearchResult(hits=[(2626778328, 19.5), (9, 1.0)], elapsed=timedelta(microseconds=1500))
    resp = api.to_response("attention", 2, result, "openalex")
    assert resp.corpus == "openalex"
    assert [h.id for h in resp.hits] == ["W2626778328", "W9"]
    assert resp.took_ms == 1.5


def test_msmarco_response_names_its_corpus_and_keeps_bare_ids():
    result = SearchResult(hits=[(2626778328, 19.5)], elapsed=timedelta(microseconds=1500))
    resp = api.to_response("q", 1, result, "msmarco")
    assert resp.corpus == "msmarco"
    assert [h.id for h in resp.hits] == ["2626778328"]


def test_profile_comes_from_the_environment(monkeypatch):
    import importlib
    monkeypatch.setenv("STARTORCH_PROFILE", "full-en")
    try:
        assert importlib.reload(api).PROFILE == "full-en"
    finally:
        monkeypatch.delenv("STARTORCH_PROFILE")
        importlib.reload(api)
        api.PROFILE = "msmarco"
