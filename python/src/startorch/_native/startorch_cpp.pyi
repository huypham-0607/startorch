"""Type stub for the pybind11 module built from cpp/python/bindings.cpp.

The compiled module carries no type information, so editors and type checkers
read this file instead. Hand-maintained: update it whenever bindings.cpp changes.
"""

from datetime import timedelta
from os import PathLike

_Path = str | PathLike[str]

class file_names:
    """Centralized on-disk naming convention (a submodule at runtime)."""
    BLOCK_META: str
    METADATA_TXT: str
    METADATA_BIN: str
    DOC_LEN_LIST: str
    DOC_LEN_META: str
    @staticmethod
    def posting_file_name(file_index: int) -> str: ...
    @staticmethod
    def partial_block_file_name(block_index: int) -> str: ...

def build_index(
    token_dir: _Path,
    partial_dir: _Path,
    out_dir: _Path,
    k1: float = ...,
    b: float = ...,
    block_size: int = ...,
    split_size: int = ...,
    mem_limit: int = ...,
) -> None:
    """Build a complete index from token_dir, reading it once. Raises ValueError
    for out-of-range parameters (defaults and ranges live in BuildParams)."""
    ...

class QueryEngine:
    """An opened index. Loads it once; query repeatedly."""
    def __init__(self, meta_path: _Path) -> None: ...
    def query(self, terms: list[str], k: int) -> tuple[list[tuple[float, int]], timedelta]:
        """Block-Max WAND top-k: ((score, mapped doc id) pairs best first, search time)."""
        ...
    def query_exhaustive(self, terms: list[str], k: int) -> tuple[list[tuple[float, int]], timedelta]:
        """Same as query, scoring every candidate with no pruning."""
        ...

def read_term_df_mapping(meta_path: _Path) -> list[tuple[str, int]]:
    """Every indexed term with its document frequency, descending by df."""
    ...
