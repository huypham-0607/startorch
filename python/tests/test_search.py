"""Searcher against the real msmarco index: results, rank safety, thread safety, GIL.

Read-only: every test only loads and queries the index under /data.
"""

import threading
import time
from concurrent.futures import ThreadPoolExecutor

import pytest

from startorch import Searcher, profile

pytestmark = pytest.mark.index


def longest_pause_while(fn) -> float:
    """Runs fn while another Python thread ticks every 5 ms; returns the longest gap.

    If fn holds the GIL, the ticker cannot run, and the gap is as long as fn.
    """
    ticks, done = [], threading.Event()

    def tick():
        while not done.is_set():
            ticks.append(time.perf_counter())
            time.sleep(0.005)

    ticker = threading.Thread(target=tick)
    ticker.start()
    time.sleep(0.05)
    fn()
    time.sleep(0.05)
    done.set()
    ticker.join()
    return max(b - a for a, b in zip(ticks, ticks[1:]))


def test_stopword_only_and_unknown_queries_have_no_hits(searcher):
    for query in ["the", "the and of", "!!!", "zzqxjvw"]:
        assert searcher.search(query, 10).hits == []


def test_hits_are_best_first_with_integer_ids(searcher):
    hits = searcher.search("ocean temperature", 20).hits
    assert len(hits) == 20
    assert all(isinstance(raw_id, int) for raw_id, _ in hits)
    scores = [score for _, score in hits]
    assert scores == sorted(scores, reverse=True)


def test_block_max_wand_matches_exhaustive_within_float_tolerance(searcher, msmarco_queries):
    # The two paths sum a document's term scores in different orders, so scores
    # can differ in the last float bits and near-ties can swap places.
    for query in msmarco_queries:
        pruned = dict(searcher.search(query, 10).hits)
        exhaustive = dict(searcher.search(query, 10, exhaustive=True).hits)
        assert pruned.keys() == exhaustive.keys(), query
        assert all(abs(pruned[d] - exhaustive[d]) < 1e-4 for d in pruned), query


def test_shared_searcher_is_thread_safe(searcher, msmarco_queries):
    """8 threads share one Searcher. Before the fix, 322 of 400 such searches failed."""
    expected = [searcher.search(q, 100).hits for q in msmarco_queries]
    with ThreadPoolExecutor(8) as pool:
        for _ in range(3):
            assert list(pool.map(lambda q: searcher.search(q, 100).hits, msmarco_queries)) == expected


def test_loading_the_index_releases_the_gil():
    loaded = []
    pause = longest_pause_while(lambda: loaded.append(Searcher(profile("msmarco"))))
    loaded[0].close()
    assert pause < 0.1, f"another thread stalled {pause:.3f}s during the load"


def test_searching_releases_the_gil(searcher):
    # About 175 ms in C++, as a single call.
    slow_query = "typing average words per minute test"
    pause = longest_pause_while(lambda: searcher.search(slow_query, 1000, exhaustive=True))
    assert pause < 0.05, f"another thread stalled {pause:.3f}s during one search"
