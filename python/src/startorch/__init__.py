"""Startorch's public API: scripts, benchmarks and notebooks import from here.

Modules inside the package import from the module that defines a name
instead (startorch.utils, startorch.paths, ...), since this file imports them
and a cycle through it depends on import order.
"""

from .utils import get_logger, get_current_time, PROJECT_ROOT
from ._native import startorch_cpp
from .paths import load_config, profile, profile_names, corpus_paths, Profile
from .ingest.fetch_data import EntityIngestor
from .works_subset.works_subset import WorksSubsetter
from .tokenizer.tokenizer import Tokenizer
from .build_posting.build_posting import PostingBuilder
from .doc_id_lookup import DocIdLookup
from .search import Searcher, SearchResult, read_query_file

# The package ships py.typed, so type checkers treat only these names as
# exported; anything imported above but left out here is private.
__all__ = [
    "get_logger", "get_current_time", "PROJECT_ROOT",
    "startorch_cpp",
    "load_config", "profile", "profile_names", "corpus_paths", "Profile",
    "EntityIngestor",
    "WorksSubsetter",
    "Tokenizer",
    "PostingBuilder",
    "DocIdLookup",
    "Searcher", "SearchResult", "read_query_file",
]
