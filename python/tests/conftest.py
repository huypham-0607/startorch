"""Shared fixtures for the Python test suite.

Run from python/:

    uv run pytest

Tests marked `index` read the msmarco index under /data (never write to it) and
are skipped when it has not been built, so the rest of the suite runs anywhere.
"""

import pytest

from startorch import Searcher, profile, read_query_file

MSMARCO = profile("msmarco")
HAS_MSMARCO_INDEX = MSMARCO.meta_path.is_file() and MSMARCO.lookup_file.is_file()


def pytest_collection_modifyitems(config, items):
    """Skips every `index` test when the msmarco index is not built."""
    if HAS_MSMARCO_INDEX:
        return
    skip = pytest.mark.skip(reason=f"no msmarco index at {MSMARCO.meta_path}")
    for item in items:
        if "index" in item.keywords:
            item.add_marker(skip)


@pytest.fixture(scope="session", autouse=True)
def serve_msmarco():
    """Pins the API to the msmarco profile, whatever STARTORCH_PROFILE says in the shell."""
    from startorch.api import api
    saved = api.PROFILE
    api.PROFILE = "msmarco"
    yield
    api.PROFILE = saved


@pytest.fixture(scope="session")
def searcher():
    """One loaded msmarco Searcher, shared by every test that needs it."""
    s = Searcher(MSMARCO)
    yield s
    s.close()


@pytest.fixture(scope="session")
def msmarco_queries():
    """The first 200 MS MARCO dev queries, as text."""
    return [text for _, text in read_query_file(MSMARCO.query_dir / "queries.dev.small.tsv", 200)]


@pytest.fixture
def anyio_backend():
    """Runs @pytest.mark.anyio tests on asyncio only (trio is not installed)."""
    return "asyncio"
