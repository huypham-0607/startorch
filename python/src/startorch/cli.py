"""
    Supports 4 commands:
        - fetch_data
        - subset
        - build_posting (which also includes tokenizer)
        - query
    
"""

import argparse
import tomllib

from pathlib import Path
from startorch import EntityIngestor, WorksSubsetter, PROJECT_ROOT, get_logger, PostingBuildler, retrieve

logger = get_logger(__name__)
CONFIG_PATH = PROJECT_ROOT / "project-config.toml"

def load_config() -> dict:
    with open(CONFIG_PATH, "rb") as f:
        return tomllib.load(f)

def resolve_path(raw: str) -> Path:
    p = Path(raw)
    return p if p.is_absolute() else (PROJECT_ROOT / p).resolve()

def get_profile_data_path(profile: str, config: dict) -> Path:
    paths = config["data-path"]
    return (
        resolve_path(paths["data-path"]) / paths["full-corpus-folder"] if (profile == "full-corpus")
        else resolve_path(paths["data-path"]) / paths["works-subset-folder"] / profile
    )

def get_profile_posting_path(profile: str, config: dict) -> Path:
    paths = config["data-path"]
    return resolve_path(paths["posting-path"]) / profile

def cmd_ingest(args: argparse.Namespace, config: dict) -> None:
    paths = config["data-path"]
    ingestor_cls = EntityIngestor.registry[args.entity]
    ingestor = ingestor_cls(
        upstream_prefix=Path(paths["upstream-path"]),
        raw_path=resolve_path(paths["data-path"]) / paths["tmp-corpus-folder"],
        compact_path=resolve_path(paths["data-path"]) / paths["full-corpus-folder"],
    )
    ingestor.orchestrate(forced_fetch=args.forced_fetch)

def cmd_gen_works_subset(args: argparse.Namespace, config: dict) -> None:
    paths = config["data-path"]
    full_corpus_path = get_profile_data_path("full-corpus", config)
    subset_path = get_profile_data_path(args.profile, config)
    spill_path = resolve_path(config["duckdb"]["spill-path"])
    condition = config["works-subset"]["subset-profiles"][args.profile]
    subsetter = WorksSubsetter(full_corpus_path, subset_path, spill_path, condition)
    subsetter.subset_database()
    subsetter.validate_database()

def cmd_build_posting(args: argparse.Namespace, config: dict) -> None:
    corpus_path = get_profile_data_path(args.profile, config)
    out_path = get_profile_posting_path(args.profile, config)
    spill_path = resolve_path(config["duckdb"]["spill-path"])
    posting_config = config["posting"]
    posting_builder = PostingBuildler(
        corpus_path,
        out_path,
        spill_path,
        posting_config["lookup-folder"],
        posting_config["lookup-file-name"],
        posting_config["token-stream-folder"],
        posting_config["posting-folder"],
        posting_config["partial-folder"],
    )
    posting_builder.build()

def cmd_query(args: argparse.Namespace, config: dict) -> None:
    root_path = get_profile_posting_path(args.profile, config)
    posting_folder = config["posting"]["posting-folder"] 
    lookup_folder = config["posting"]["lookup-folder"] 
    lookup_file_name = config["posting"]["lookup-file-name"] 
    retrieve(
        args.query,
        args.k,
        root_path,
        posting_folder,
        lookup_folder,
        lookup_file_name
    )

def build_parser(config: dict) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="startorch")
    subparsers = parser.add_subparsers(dest="command", required = True)

    # ingest
    ingest_parser = subparsers.add_parser("ingest", help="Fetch + extract OpenAlex shards")
    ingest_parser.add_argument(
        "--entity",
        required=True,
        choices=sorted(EntityIngestor.registry.keys()),
        help="Which OpenAlex entity to ingest.",
    )
    ingest_parser.add_argument(
        "--forced-fetch",
        help="Force a refetch regardless if compact shards are present.",
        action="store_true"
    )
    ingest_parser.set_defaults(func=cmd_ingest)

    # gen_works_subset
    gen_works_subset_parser = subparsers.add_parser("gen-works-subset", help="Extract works subset from full corpus.")
    gen_works_subset_parser.add_argument(
        "--profile",
        required=True,
        choices=sorted(config["works-subset"]["subset-profiles"]),
        help="Subset profile to use (see project-config.toml for filter conditions)."
    )
    gen_works_subset_parser.set_defaults(func=cmd_gen_works_subset)

    # build_posting
    build_posting_parser = subparsers.add_parser("build-posting", help="Build inverted index posting from data.")
    build_posting_parser.add_argument(
        "--profile",
        required=True,
        choices=sorted(config["works-subset"]["subset-profiles"]),
        help="Subset profile to use (see project-config.toml for filter conditions), has to be generated first."
    )
    build_posting_parser.set_defaults(func=cmd_build_posting)

    # query
    query_parser = subparsers.add_parser("query", help="Retrieve top-k documents relevant to a query")
    query_parser.add_argument(
        "--query",
        required=True,
        help="The query string to evaluate."
    )
    query_parser.add_argument(
        "--k",
        type=int,
        default=10,
        help="Number of documents to retrieve, defaults to 10."
    )
    query_parser.add_argument(
        "--profile",
        default="full-en",
        choices=sorted(config["works-subset"]["subset-profiles"]),
        help="Subset profile to query (Defaults to full-en)."
    )
    query_parser.set_defaults(func=cmd_query)

    return parser


def main():
    config = load_config()
    parser = build_parser(config)
    args = parser.parse_args()
    args.func(args, config)


if __name__ == "__main__":
    main()