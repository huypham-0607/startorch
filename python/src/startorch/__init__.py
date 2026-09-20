"""Startorch's public API: scripts, benchmarks and notebooks import from here.

Modules inside the package import from the module that defines a name instead
(startorch.utils.logger, startorch.lexical.search, ...), since this file
imports them and a cycle through it depends on import order.
"""

from ._native import startorch_cpp
from .utils.logger import get_logger
from .utils.misc import get_current_time, PROJECT_ROOT
from .utils.paths import load_config, profile, profile_names, corpus_paths, Profile
from .ingest.fetch_data import EntityIngestor
from .ingest.works_subset import WorksSubsetter
from .lexical.tokenizer import Tokenizer
from .lexical.build_posting import PostingBuilder
from .lexical.doc_id_lookup import DocIdLookup
from .lexical.search import Searcher, SearchResult, read_query_file

# The package ships py.typed, so type checkers treat only these names as
# exported; anything imported above but left out here is private.
__all__ = [
    "startorch_cpp",
    "get_logger", "get_current_time", "PROJECT_ROOT",
    "load_config", "profile", "profile_names", "corpus_paths", "Profile",
    "EntityIngestor",
    "WorksSubsetter",
    "Tokenizer",
    "PostingBuilder",
    "DocIdLookup",
    "Searcher", "SearchResult", "read_query_file",
]
