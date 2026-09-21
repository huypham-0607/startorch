"""The doc_id lookup file: dense mapped_id <-> raw id (OpenAlex id, or MS MARCO pid).

doc_id_lookup.bin holds one 12-byte record per document, sorted by raw_id: a
little-endian int64 raw_id followed by an int32 mapped_id. mapped_id is the
record's position (the raw id's rank), so the whole file is a sorted array.
This module holds the only definition of that layout.

Attributes:
    RECORD: The numpy dtype of one record: raw_id (<i8), then mapped_id (<i4).
"""

import numpy as np

from pathlib import Path

RECORD = np.dtype([("raw_id", "<i8"), ("mapped_id", "<i4")])


def lookup_records(raw_ids: np.ndarray, start: int) -> np.ndarray:
    """Builds lookup records for a run of ascending raw ids.

    Args:
        raw_ids: Consecutive raw ids, already in ascending order.
        start: The mapped id of the first raw id in the run.

    Returns:
        One RECORD per raw id, with mapped ids start, start + 1, and so on.
    """
    records = np.empty(len(raw_ids), dtype=RECORD)
    records["raw_id"] = raw_ids
    records["mapped_id"] = np.arange(start, start + len(raw_ids), dtype=np.int32)
    return records


def map_raw_ids(sorted_raw_ids: np.ndarray, raw_ids: np.ndarray) -> np.ndarray:
    """Maps raw ids to mapped ids by binary search over the sorted raw ids.

    Args:
        sorted_raw_ids: Every raw id in the lookup, ascending, so each position
            is that raw id's mapped id.
        raw_ids: The raw ids to map, in any order.

    Returns:
        Each raw id's mapped id, aligned with raw_ids.

    Raises:
        KeyError: If any raw id is not in the lookup.
    """
    mapped = np.searchsorted(sorted_raw_ids, raw_ids)
    found = mapped < len(sorted_raw_ids)
    if not (found.all() and np.array_equal(sorted_raw_ids[mapped], raw_ids)):
        missing = np.asarray(raw_ids)[~found | (sorted_raw_ids[np.minimum(mapped, len(sorted_raw_ids) - 1)] != raw_ids)]
        raise KeyError(f"{len(missing)} raw ids are not in the doc_id lookup, e.g. {missing[:5].tolist()}.")
    return mapped


class DocIdLookup:
    """Read-only, memory-mapped view of a doc_id_lookup.bin.

    raw_ids(mapped) is the query direction: it touches only the records asked
    for. mapped_ids(raw) is the bulk direction: it copies the raw-id column into
    a contiguous array on first use (about 2.8 GB on full-en).

    Attributes:
        path: The lookup file this view reads.
    """

    def __init__(self, path: Path):
        """Opens a lookup file.

        Args:
            path: Path to a doc_id_lookup.bin.

        Raises:
            FileNotFoundError: If the file does not exist.
            ValueError: If the file size is not a whole number of records.
        """
        self.path = Path(path)
        size = self.path.stat().st_size
        if size % RECORD.itemsize != 0:
            raise ValueError(f"{self.path} is {size} bytes, not a whole number of {RECORD.itemsize}-byte records.")
        self._records = np.memmap(self.path, dtype=RECORD, mode="r") if size else np.empty(0, dtype=RECORD)
        self._sorted_raw_ids = None

    def __len__(self) -> int:
        """Returns the number of documents in the lookup."""
        return len(self._records)

    def raw_ids(self, mapped_ids) -> np.ndarray:
        """Maps dense mapped ids back to raw ids.

        Args:
            mapped_ids: Mapped ids, as any integer array-like.

        Returns:
            The raw id for each mapped id, aligned with the input.

        Raises:
            IndexError: If a mapped id is outside the lookup.
        """
        return np.asarray(self._records["raw_id"][np.asarray(mapped_ids, dtype=np.int64)])

    def mapped_ids(self, raw_ids) -> np.ndarray:
        """Maps raw ids to dense mapped ids.

        The first call copies the raw-id column into memory; later calls reuse it.

        Args:
            raw_ids: Raw ids, as any integer array-like.

        Returns:
            The mapped id for each raw id, aligned with the input.

        Raises:
            KeyError: If any raw id is not in the lookup.
        """
        if self._sorted_raw_ids is None:
            self._sorted_raw_ids = np.ascontiguousarray(self._records["raw_id"])
        return map_raw_ids(self._sorted_raw_ids, np.asarray(raw_ids, dtype=np.int64))
