"""Profile name -> every artifact path and build parameter, from project-config.toml.

The only place data paths are built. Everything else asks profile(name), so a
folder name or layout change happens here once.

Attributes:
    CONFIG_PATH: Location of project-config.toml, at the repository root.
"""

import tomllib

from dataclasses import dataclass, field
from functools import cache
from pathlib import Path

from startorch.utils.misc import PROJECT_ROOT
from startorch._native import startorch_cpp

CONFIG_PATH = PROJECT_ROOT / "project-config.toml"

# project-config.toml key -> startorch_cpp.build_index keyword.
_BUILD_KEYS = {"k1": "k1", "b": "b", "block-size": "block_size", "split-size": "split_size", "mem-limit": "mem_limit"}


@cache
def load_config() -> dict:
    """Reads project-config.toml once and caches the parsed result.

    Returns:
        The parsed TOML as a nested dict. Later calls return the same object.
    """
    with open(CONFIG_PATH, "rb") as f:
        return tomllib.load(f)


def _resolve(raw: str) -> Path:
    """Resolves a configured path against the repository root.

    Args:
        raw: A path from project-config.toml, absolute or relative to the root.

    Returns:
        The path unchanged if absolute, otherwise resolved under PROJECT_ROOT.
    """
    p = Path(raw)
    return p if p.is_absolute() else (PROJECT_ROOT / p).resolve()


@dataclass(frozen=True)
class CorpusPaths:
    """Where ingest reads and writes the OpenAlex corpus.

    Attributes:
        upstream: Snapshot prefix in the OpenAlex S3 bucket.
        raw: Folder for downloaded shards, deleted after compaction.
        compact: Compacted corpus root, the folder that holds works/.
    """
    upstream: Path
    raw: Path
    compact: Path

    def works_glob(self) -> str:
        """Returns a glob matching every compacted Works parquet file.

        Returns:
            A recursive glob string, suitable for DuckDB's read_parquet.
        """
        return str(self.compact / "works" / "**" / "*.parquet")


@dataclass(frozen=True)
class Profile:
    """Every path and build parameter for one profile in project-config.toml.

    Built by profile(name). Fields marked OpenAlex or MS MARCO are None (or
    False) for the other kind.

    Attributes:
        name: The profile's table name in project-config.toml.
        kind: "openalex" or "msmarco".
        filter: OpenAlex only. The SQL condition selecting this profile's works.
        materialize: OpenAlex only. True if the works are copied into subset_dir.
        subset_dir: OpenAlex only. works_subset/<name>, whether or not it exists yet.
        collection: MS MARCO only. Path to collection.tsv.
        index_root: Root folder of this profile's index artifacts.
        lookup_file: doc_id_lookup.bin, mapping raw ids to dense mapped ids.
        token_dir: Folder of token_*.bin files, the C++ build's input.
        posting_dir: The finished index: posting files, block metadata, metadata.bin.
        partial_dir: SPIMI partial blocks, an intermediate of the build.
        query_dir: Benchmark query sets for this profile.
        result_dir: Benchmark results for this profile.
        spill_dir: DuckDB's spill directory for work that exceeds memory.
        build_params: The build_index keyword arguments this profile overrides.
    """
    name: str
    kind: str
    filter: str | None
    materialize: bool
    subset_dir: Path | None
    collection: Path | None
    index_root: Path
    lookup_file: Path
    token_dir: Path
    posting_dir: Path
    partial_dir: Path
    query_dir: Path
    result_dir: Path
    spill_dir: Path
    build_params: dict = field(default_factory=dict)

    @property
    def meta_path(self) -> Path:
        """Path to the index's metadata.bin, the file QueryEngine opens."""
        return self.posting_dir / startorch_cpp.file_names.METADATA_BIN


def corpus_paths() -> CorpusPaths:
    """Returns where ingest reads and writes the OpenAlex corpus.

    Returns:
        CorpusPaths built from the [paths] and [folders] tables of project-config.toml.
    """
    config = load_config()
    data_root = _resolve(config["paths"]["data-root"])
    return CorpusPaths(
        upstream=Path(config["paths"]["upstream"]),
        raw=data_root / config["folders"]["tmp-corpus"],
        compact=data_root / config["folders"]["full-corpus"],
    )


def profile_names(kind: str | None = None) -> list[str]:
    """Lists profile names, optionally only those of one kind.

    Args:
        kind: "openalex" or "msmarco" to filter by kind, or None for every profile.

    Returns:
        Profile names, sorted alphabetically.
    """
    profiles = load_config()["profiles"]
    return sorted(name for name, table in profiles.items() if kind is None or table["kind"] == kind)


def profile(name: str) -> Profile:
    """Builds the Profile for one [profiles.<name>] table in project-config.toml.

    Paths a profile does not set itself are derived from [paths] and [folders],
    so a layout change only touches the config.

    Args:
        name: The profile name, e.g. "full-en" or "msmarco".

    Returns:
        The profile's paths and build parameters.

    Raises:
        KeyError: If no profile has that name.
        ValueError: If the profile's kind is neither "openalex" nor "msmarco".
    """
    config = load_config()
    if name not in config["profiles"]:
        raise KeyError(f"Unknown profile {name!r}. Known profiles: {', '.join(profile_names())}.")
    table = config["profiles"][name]
    paths, folders = config["paths"], config["folders"]

    kind = table["kind"]
    if kind not in ("openalex", "msmarco"):
        raise ValueError(f"Profile {name!r} has unknown kind {kind!r}.")

    index_root = _resolve(table["index-root"]) if "index-root" in table else _resolve(paths["index-root"]) / name
    posting_dir = index_root / folders["posting"]
    is_openalex = kind == "openalex"

    return Profile(
        name=name,
        kind=kind,
        filter=table["filter"] if is_openalex else None,
        materialize=bool(table.get("materialize", False)) if is_openalex else False,
        subset_dir=_resolve(paths["data-root"]) / folders["works-subset"] / name if is_openalex else None,
        collection=_resolve(table["collection"]) if kind == "msmarco" else None,
        index_root=index_root,
        lookup_file=index_root / folders["lookup"] / config["files"]["lookup"],
        token_dir=index_root / folders["token-stream"],
        posting_dir=posting_dir,
        partial_dir=index_root / folders["partial"],
        query_dir=_resolve(paths["query-root"]) / name,
        result_dir=_resolve(paths["benchmark-root"]) / name / folders["query-result"],
        spill_dir=_resolve(paths["spill"]),
        build_params={kw: table[key] for key, kw in _BUILD_KEYS.items() if key in table},
    )
