"""DocIdLookup: the raw id <-> dense mapped id file."""

import numpy as np
import pytest

from startorch.lexical.doc_id_lookup import RECORD, DocIdLookup, lookup_records

RAW = np.array([3, 17, 42, 1000, 2**40], dtype=np.int64)


@pytest.fixture
def lookup(tmp_path):
    path = tmp_path / "doc_id_lookup.bin"
    path.write_bytes(lookup_records(RAW, 0).tobytes())
    return DocIdLookup(path)


def test_round_trips_raw_and_mapped_ids(lookup):
    assert len(lookup) == len(RAW)
    assert lookup.raw_ids([4, 0, 2]).tolist() == [2**40, 3, 42]
    assert lookup.mapped_ids([42, 3]).tolist() == [2, 0]


def test_unknown_raw_id_raises_key_error(lookup):
    with pytest.raises(KeyError):
        lookup.mapped_ids([5])


def test_rejects_a_file_that_is_not_whole_records(tmp_path):
    path = tmp_path / "bad.bin"
    path.write_bytes(b"\0" * (RECORD.itemsize + 1))
    with pytest.raises(ValueError):
        DocIdLookup(path)


def test_empty_file_is_an_empty_lookup(tmp_path):
    path = tmp_path / "empty.bin"
    path.write_bytes(b"")
    assert len(DocIdLookup(path)) == 0
