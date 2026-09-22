"""Type stub for the pybind11 module built from cpp/python/bindings.cpp.

The compiled module carries no type information, so editors and type checkers
read this file instead. Hand-maintained: update it whenever bindings.cpp changes.
"""

from datetime import timedelta
from os import PathLike

_Path = str | PathLike[str]

class file_names:
    """The on-disk file names shared with the C++ build. A submodule at runtime.

    Attributes:
        BLOCK_META: Block-Max WAND block metadata file name.
        METADATA_TXT: Human-readable index metadata file name.
        METADATA_BIN: Binary index metadata file name, the file QueryEngine opens.
        DOC_LEN_LIST: Document-length list file name.
        DOC_LEN_META: Document-length summary file name.
    """
    BLOCK_META: str
    METADATA_TXT: str
    METADATA_BIN: str
    DOC_LEN_LIST: str
    DOC_LEN_META: str
    @staticmethod
    def posting_file_name(file_index: int) -> str:
        """Returns the name of the file_index-th posting file."""
        ...
    @staticmethod
    def partial_block_file_name(block_index: int) -> str:
        """Returns the name of the block_index-th SPIMI partial block."""
        ...

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
    """Builds a complete index from a folder of token files, reading them once.

    Keyword defaults and valid ranges live in BuildParams on the C++ side.

    Args:
        token_dir: Folder of token_*.bin files, from the tokenizer.
        partial_dir: Folder for SPIMI partial blocks. Leftover blocks are removed first.
        out_dir: Folder for the finished index.
        k1: BM25 term-frequency saturation.
        b: BM25 document-length normalization.
        block_size: Postings per Block-Max WAND block.
        split_size: Maximum bytes per posting file.
        mem_limit: SPIMI memory budget per partial block, in bytes.

    Raises:
        ValueError: If a parameter is out of range.
        RuntimeError: If a file cannot be read or written.
    """
    ...

class QueryEngine:
    """An opened index: loaded once, then queried repeatedly.

    Thread-safe: any number of threads may call query() and query_exhaustive()
    on one engine at once. The constructor and both queries release the GIL
    while they run, so other Python threads keep running meanwhile.
    """
    def __init__(self, meta_path: _Path) -> None:
        """Loads the index whose metadata.bin is at meta_path.

        Args:
            meta_path: Path to the index's metadata.bin.

        Raises:
            RuntimeError: If an index file is missing, or the metadata format is outdated.
        """
        ...
    def query(self, terms: list[str], k: int) -> tuple[list[tuple[float, int]], timedelta]:
        """Runs a Block-Max WAND top-k BM25 search.

        Args:
            terms: Query tokens.
            k: Number of results to return.

        Returns:
            (score, mapped doc id) pairs best first, and the search time.
        """
        ...
    def query_exhaustive(self, terms: list[str], k: int) -> tuple[list[tuple[float, int]], timedelta]:
        """Scores every candidate with no pruning. The ground truth for query().

        Args:
            terms: Query tokens.
            k: Number of results to return.

        Returns:
            The same shape as query(); the results must match it exactly.
        """
        ...

def read_term_df_mapping(meta_path: _Path) -> list[tuple[str, int]]:
    """Lists every indexed term with its document frequency.

    Args:
        meta_path: Path to the index's metadata.bin.

    Returns:
        (term, document frequency) pairs, highest frequency first.

    Raises:
        RuntimeError: If an index file is missing, or the metadata format is outdated.
    """
    ...
