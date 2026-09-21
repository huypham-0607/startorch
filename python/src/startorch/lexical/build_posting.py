"""Builds one profile's index: tokenize in Python, then build_index in C++."""

from startorch._native import startorch_cpp
from startorch.utils.paths import Profile
from startorch.lexical.tokenizer import Tokenizer, profile_source
from startorch.utils.logger import get_logger

logger = get_logger(__name__)


class PostingBuilder:
    """Builds a profile's index from its source documents.

    Paths and build parameters come from the profile's project-config.toml table
    (see startorch.utils.paths); parameters it doesn't set take BuildParams'
    defaults in C++.

    Attributes:
        profile: The profile to build.
    """

    def __init__(self, profile: Profile) -> None:
        """Stores the profile to build; nothing is read until build().

        Args:
            profile: The profile to build, from startorch.utils.paths.profile().
        """
        self.profile = profile

    def build(self) -> None:
        """Tokenizes the profile's documents, then builds the index in one C++ pass.

        The tokenizer's DuckDB connection is closed before the C++ build starts,
        so its memory is not held through the build.

        Raises:
            ValueError: If a build parameter is out of range, or a token is
                longer than 65,535 bytes.
            RuntimeError: If the C++ build cannot read or write its files.
        """
        p = self.profile
        tokenizer = Tokenizer()
        try:
            tokenizer.get_token(profile_source(p), p.token_dir, p.lookup_file, p.spill_dir)
        finally:
            # DuckDB's memory would otherwise stay held through the C++ build.
            tokenizer.close()

        logger.info(f"Building index for {p.name} into {p.posting_dir} with {p.build_params or 'default parameters'}.")
        startorch_cpp.build_index(p.token_dir, p.partial_dir, p.posting_dir, **p.build_params)
