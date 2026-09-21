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
    """The outcome of one search.

    Attributes:
        hits: (raw_id, score) pairs, best first. Raw ids are OpenAlex ids or MS MARCO pids.
        elapsed: C++ search time only, excluding tokenization and id mapping.
    """
    hits: list[tuple[int, float]]
    elapsed: timedelta


class Searcher:
    """One profile's index, loaded once, answering many queries.

    Construction loads the whole index (about 14 s and 6.7 GiB of memory on
    full-en). Results carry raw ids (OpenAlex ids, or MS MARCO pids), mapped
    back through the profile's doc_id lookup.

    Attributes:
        profile: The profile whose index is loaded.
        tokenizer: Tokenizes queries exactly as the indexed documents were.
        engine: The C++ QueryEngine holding the loaded index.
        load_latency: How long loading the index took.
        lookup: Maps result ids back to raw ids.
    """

    def __init__(self, profile: Profile):
        """Loads a profile's index and its doc_id lookup.

        Args:
            profile: The profile to search, from startorch.utils.paths.profile().

        Raises:
            RuntimeError: If an index file is missing or in an outdated format.
            FileNotFoundError: If the doc_id lookup file is missing.
        """
        self.profile = profile
        self.tokenizer = Tokenizer()
        start = time.perf_counter()
        self.engine = startorch_cpp.QueryEngine(profile.meta_path)
        self.load_latency = timedelta(seconds=time.perf_counter() - start)
        self.lookup = DocIdLookup(profile.lookup_file)

    def search_terms(self, terms: list[str], k: int, exhaustive: bool = False) -> SearchResult:
        """Searches already-tokenized terms.

        Args:
            terms: Query tokens, as produced by Tokenizer.tokenize_query.
            k: Number of results to return.
            exhaustive: If True, score every candidate with no pruning. That is
                the ground truth the Block-Max WAND search must match.

        Returns:
            The top-k hits and the C++ search time.
        """
        run = self.engine.query_exhaustive if exhaustive else self.engine.query
        results, elapsed = run(terms, k)
        raw_ids = self.lookup.raw_ids([mapped_id for _, mapped_id in results]) if results else []
        return SearchResult(
            hits=[(int(raw_id), score) for raw_id, (score, _) in zip(raw_ids, results)],
            elapsed=elapsed,
        )

    def search(self, query: str, k: int, exhaustive: bool = False) -> SearchResult:
        """Tokenizes a query string and searches it.

        Args:
            query: Free-text query.
            k: Number of results to return.
            exhaustive: If True, score every candidate with no pruning.

        Returns:
            The top-k hits and the C++ search time.
        """
        return self.search_terms(self.tokenizer.tokenize_query(query), k, exhaustive)

    def search_many(self, queries: list[str], k: int, exhaustive: bool = False) -> list[SearchResult]:
        """Searches each query in turn against the same loaded index.

        Args:
            queries: Free-text queries.
            k: Number of results per query.
            exhaustive: If True, score every candidate with no pruning.

        Returns:
            One SearchResult per query, in input order.
        """
        return [self.search(query, k, exhaustive) for query in queries]


def read_query_file(path: Path, cap: int | None = None) -> list[tuple[str, str]]:
    """Reads a tab-separated query set.

    Args:
        path: A TSV file of (query id, query text) rows, with no header.
        cap: If set, keep only the first cap rows.

    Returns:
        (query id, query text) pairs, in file order.

    Raises:
        ValueError: If a row does not have exactly two fields.
    """
    with open(path, mode="r", encoding="utf-8") as f:
        rows = [(qid, text) for qid, text in csv.reader(f, delimiter="\t")]
    return rows if cap is None else rows[:cap]
