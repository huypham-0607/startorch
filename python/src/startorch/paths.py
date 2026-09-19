"""Profile name -> every artifact path and build parameter, from project-config.toml.

The only place data paths are built. Everything else asks profile(name), so a
folder name or layout change happens here once.
"""

import tomllib

from dataclasses import dataclass, field
from functools import cache
from pathlib import Path

from startorch.utils import PROJECT_ROOT
from startorch._native import startorch_cpp

CONFIG_PATH = PROJECT_ROOT / "project-config.toml"

# project-config.toml key -> startorch_cpp.build_index keyword.
_BUILD_KEYS = {"k1": "k1", "b": "b", "block-size": "block_size", "split-size": "split_size", "mem-limit": "mem_limit"}


@cache
def load_config() -> dict:
    """project-config.toml, read once."""
    with open(CONFIG_PATH, "rb") as f:
        return tomllib.load(f)


def _resolve(raw: str) -> Path:
    p = Path(raw)
    return p if p.is_absolute() else (PROJECT_ROOT / p).resolve()


@dataclass(frozen=True)
class CorpusPaths:
    """Where ingest writes the OpenAlex corpus."""
    upstream: Path      # snapshot prefix on S3
    raw: Path           # downloaded shards, deleted after compaction
    compact: Path       # compacted corpus root (holds works/)

    def works_glob(self) -> str:
        return str(self.compact / "works" / "**" / "*.parquet")


@dataclass(frozen=True)
class Profile:
    name: str
    kind: str                   # "openalex" or "msmarco"
    filter: str | None          # OpenAlex: the condition selecting this profile's works
    materialize: bool           # OpenAlex: works are copied into subset_dir
    subset_dir: Path | None     # OpenAlex: works_subset/<name>, whether or not it exists yet
    collection: Path | None     # MS MARCO: collection.tsv
    index_root: Path
    lookup_file: Path
    token_dir: Path
    posting_dir: Path
    partial_dir: Path
    query_dir: Path
    result_dir: Path
    spill_dir: Path
    build_params: dict = field(default_factory=dict)   # build_index keywords this profile sets

    @property
    def meta_path(self) -> Path:
        return self.posting_dir / startorch_cpp.file_names.METADATA_BIN


def corpus_paths() -> CorpusPaths:
    config = load_config()
    data_root = _resolve(config["paths"]["data-root"])
    return CorpusPaths(
        upstream=Path(config["paths"]["upstream"]),
        raw=data_root / config["folders"]["tmp-corpus"],
        compact=data_root / config["folders"]["full-corpus"],
    )


def profile_names(kind: str | None = None) -> list[str]:
    """Every profile, or only those of one kind ("openalex" or "msmarco")."""
    profiles = load_config()["profiles"]
    return sorted(name for name, table in profiles.items() if kind is None or table["kind"] == kind)


def profile(name: str) -> Profile:
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
