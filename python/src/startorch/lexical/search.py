"""Search one profile's index and get results back, not printed.

The CLI prints Searcher results; the benchmarks time them. Both go through here.
"""

import csv
import time

from dataclasses import dataclass
from datetime import timedelta
from pathlib import Path

from startorch._native import startorch_cpp
from startorch.lexical.doc_id_lookup import DocIdLookup
from startorch.utils.paths import Profile
from startorch.lexical.tokenizer import Tokenizer


@dataclass(frozen=True)
class SearchResult:
    hits: list[tuple[int, float]]   # (raw_id, score), best first
    elapsed: timedelta              # C++ search time only: no tokenizing or id mapping


class Searcher:
    """One profile's index, loaded once, answering many queries.

    Construction loads the whole index (about 96 s on full-en); load_latency
    records it. Results carry raw ids (OpenAlex ids, or MS MARCO pids),
    mapped back through the profile's doc_id lookup.
    """

    def __init__(self, profile: Profile):
        self.profile = profile
        self.tokenizer = Tokenizer()
        start = time.perf_counter()
        self.engine = startorch_cpp.QueryEngine(profile.meta_path)
        self.load_latency = timedelta(seconds=time.perf_counter() - start)
        self.lookup = DocIdLookup(profile.lookup_file)

    def search_terms(self, terms: list[str], k: int, exhaustive: bool = False) -> SearchResult:
        """Search already-tokenized terms. exhaustive scores every candidate
        with no pruning: the ground truth for the Block-Max WAND search."""
        run = self.engine.query_exhaustive if exhaustive else self.engine.query
        results, elapsed = run(terms, k)
        raw_ids = self.lookup.raw_ids([mapped_id for _, mapped_id in results]) if results else []
        return SearchResult(
            hits=[(int(raw_id), score) for raw_id, (score, _) in zip(raw_ids, results)],
            elapsed=elapsed,
        )

    def search(self, query: str, k: int, exhaustive: bool = False) -> SearchResult:
        return self.search_terms(self.tokenizer.tokenize_query(query), k, exhaustive)

    def search_many(self, queries: list[str], k: int, exhaustive: bool = False) -> list[SearchResult]:
        return [self.search(query, k, exhaustive) for query in queries]


def read_query_file(path: Path, cap: int | None = None) -> list[tuple[str, str]]:
    """(query id, query text) rows from a tab-separated query set, first cap rows."""
    with open(path, mode="r", encoding="utf-8") as f:
        rows = [(qid, text) for qid, text in csv.reader(f, delimiter="\t")]
    return rows if cap is None else rows[:cap]
