"""The doc_id lookup file: dense mapped_id <-> raw id (OpenAlex id, or MS MARCO pid).

doc_id_lookup.bin holds one 12-byte record per document, sorted by raw_id: a
little-endian int64 raw_id followed by an int32 mapped_id. mapped_id is the
record's position (the raw id's rank), so the whole file is a sorted array.
This module holds the only definition of that layout.
"""

import numpy as np

from pathlib import Path

RECORD = np.dtype([("raw_id", "<i8"), ("mapped_id", "<i4")])


def lookup_records(raw_ids: np.ndarray, start: int) -> np.ndarray:
    """Records for a run of ascending raw ids whose mapped ids begin at start."""
    records = np.empty(len(raw_ids), dtype=RECORD)
    records["raw_id"] = raw_ids
    records["mapped_id"] = np.arange(start, start + len(raw_ids), dtype=np.int32)
    return records


def map_raw_ids(sorted_raw_ids: np.ndarray, raw_ids: np.ndarray) -> np.ndarray:
    """mapped_id for each raw id, by binary search over the ascending raw ids.

    Raises KeyError if any raw id is not in the lookup.
    """
    mapped = np.searchsorted(sorted_raw_ids, raw_ids)
    found = mapped < len(sorted_raw_ids)
    if not (found.all() and np.array_equal(sorted_raw_ids[mapped], raw_ids)):
        missing = np.asarray(raw_ids)[~found | (sorted_raw_ids[np.minimum(mapped, len(sorted_raw_ids) - 1)] != raw_ids)]
        raise KeyError(f"{len(missing)} raw ids are not in the doc_id lookup, e.g. {missing[:5].tolist()}.")
    return mapped


class DocIdLookup:
    """Read-only view of a doc_id_lookup.bin, memory-mapped.

    raw_ids(mapped) is the query direction: it touches only the records asked
    for. mapped_ids(raw) is the bulk direction: it copies the raw-id column into
    a contiguous array on first use (about 2.8 GB on full-en).
    """

    def __init__(self, path: Path):
        self.path = Path(path)
        size = self.path.stat().st_size
        if size % RECORD.itemsize != 0:
            raise ValueError(f"{self.path} is {size} bytes, not a whole number of {RECORD.itemsize}-byte records.")
        self._records = np.memmap(self.path, dtype=RECORD, mode="r") if size else np.empty(0, dtype=RECORD)
        self._sorted_raw_ids = None

    def __len__(self) -> int:
        return len(self._records)

    def raw_ids(self, mapped_ids) -> np.ndarray:
        return np.asarray(self._records["raw_id"][np.asarray(mapped_ids, dtype=np.int64)])

    def mapped_ids(self, raw_ids) -> np.ndarray:
        if self._sorted_raw_ids is None:
            self._sorted_raw_ids = np.ascontiguousarray(self._records["raw_id"])
        return map_raw_ids(self._sorted_raw_ids, np.asarray(raw_ids, dtype=np.int64))
