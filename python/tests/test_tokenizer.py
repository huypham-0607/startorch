"""Tokenizer: query tokenization, the token-stream record layout, and thread safety.

Needs no index: tokenizing runs on an in-memory DuckDB database.
"""

import random
import threading
from concurrent.futures import ThreadPoolExecutor

import numpy as np
import pyarrow as pa
import pytest

from startorch.lexical.tokenizer import Tokenizer

WORDS = ["riemann", "hypothesis", "prime", "numbers", "the", "and", "of", "graph", "neural", "networks",
         "attention", "is", "all", "you", "need", "ocean", "temperature", "running", "ran", "runs"]


def make_queries(n: int = 200) -> list[str]:
    """Deterministic, varied queries, including some made only of stopwords."""
    rng = random.Random(7)
    return [" ".join(rng.choice(WORDS) for _ in range(rng.randint(1, 6))) for _ in range(n)]


@pytest.fixture
def tokenizer():
    t = Tokenizer()
    yield t
    t.close()


def test_tokenizes_like_the_index(tokenizer):
    assert tokenizer.tokenize_query("The Riemann hypothesis and prime numbers") == ["riemann", "hypothesi", "prime", "number"]


def test_stopword_only_query_has_no_tokens(tokenizer):
    assert tokenizer.tokenize_query("the and of") == []


def test_encodes_the_record_layout_the_cpp_build_reads():
    # int64 doc_id, uint16 byte length, then the token's bytes, per token.
    records, count, longest = Tokenizer.encode_token_records(pa.array([["ab", "c"], [], ["def"]]), np.array([5, 6, 7]))
    assert (count, longest) == (3, 3)
    assert bytes(records).hex() == "050000000000000002006162" "050000000000000001006307000000000000000300646566"


def test_rejects_a_token_too_long_for_its_length_field():
    with pytest.raises(ValueError):
        Tokenizer.encode_token_records(pa.array([["a" * 70_000]]), np.array([0]))


def test_shared_tokenizer_is_thread_safe(tokenizer):
    """8 threads share one Tokenizer. Before per-thread cursors, 1,796 of 2,000 calls failed."""
    queries = make_queries()
    expected = [tokenizer.tokenize_query(q) for q in queries]
    with ThreadPoolExecutor(8) as pool:
        for _ in range(5):
            assert list(pool.map(tokenizer.tokenize_query, queries)) == expected


def test_each_thread_gets_its_own_cursor(tokenizer):
    barrier = threading.Barrier(8)   # hold all 8 threads until each has its cursor

    def cursor_id(_):
        cursor = tokenizer._cursor()
        barrier.wait()
        return id(cursor)

    with ThreadPoolExecutor(8) as pool:
        ids = list(pool.map(cursor_id, range(8)))
    assert len(set(ids)) == 8


def test_close_closes_every_cursor():
    t = Tokenizer()
    with ThreadPoolExecutor(4) as pool:
        list(pool.map(t.tokenize_query, make_queries(20)))
    cursors = list(t._cursors)
    assert cursors, "the threads should have created cursors"
    t.close()
    for cursor in cursors:
        with pytest.raises(Exception):
            cursor.execute("SELECT 1")
