"""Orchestrate the index build for one profile: tokenize, then build_index in C++.

"""

from startorch._native import startorch_cpp
from startorch.paths import Profile
from startorch.tokenizer.tokenizer import Tokenizer, profile_source
from startorch.utils import get_logger

logger = get_logger(__name__)


class PostingBuilder:
    """Builds a profile's index. Paths and build parameters come from its
    project-config.toml table (see startorch.paths); parameters it doesn't set
    take BuildParams' defaults in C++."""

    def __init__(self, profile: Profile) -> None:
        self.profile = profile

    def build(self) -> None:
        p = self.profile
        tokenizer = Tokenizer()
        try:
            tokenizer.get_token(profile_source(p), p.token_dir, p.lookup_file, p.spill_dir)
        finally:
            # DuckDB's memory would otherwise stay held through the C++ build.
            tokenizer.close()

        logger.info(f"Building index for {p.name} into {p.posting_dir} with {p.build_params or 'default parameters'}.")
        startorch_cpp.build_index(p.token_dir, p.partial_dir, p.posting_dir, **p.build_params)
