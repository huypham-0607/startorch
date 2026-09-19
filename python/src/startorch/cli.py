"""
    Supports 4 commands:
        - ingest
        - gen-works-subset
        - build-posting (tokenizer + C++ index build)
        - query

    Every path and build parameter comes from project-config.toml through
    startorch.paths; this module only parses arguments and dispatches.
"""

import argparse

from startorch import (
    EntityIngestor, PostingBuilder, Searcher, WorksSubsetter,
    corpus_paths, get_logger, profile, profile_names,
)

logger = get_logger(__name__)

def cmd_ingest(args: argparse.Namespace) -> None:
    corpus = corpus_paths()
    ingestor_cls = EntityIngestor.registry[args.entity]
    ingestor = ingestor_cls(
        upstream_prefix=corpus.upstream,
        raw_path=corpus.raw,
        compact_path=corpus.compact,
    )
    ingestor.orchestrate(forced_fetch=args.forced_fetch)

def cmd_gen_works_subset(args: argparse.Namespace) -> None:
    p = profile(args.profile)
    # argparse only offers OpenAlex profiles here, and they always have both.
    assert p.subset_dir is not None and p.filter is not None, f"{p.name} is not an OpenAlex profile"
    subsetter = WorksSubsetter(corpus_paths().compact, p.subset_dir, p.spill_dir, p.filter)
    if not p.materialize:
        n_works = subsetter.count_matching()
        logger.info(f"Profile {p.name} reads the corpus through its filter ({n_works} works); nothing to copy.")
        return
    subsetter.subset_database()
    subsetter.validate_database()

def cmd_build_posting(args: argparse.Namespace) -> None:
    PostingBuilder(profile(args.profile)).build()

def cmd_query(args: argparse.Namespace) -> None:
    result = Searcher(profile(args.profile)).search(args.query, args.k)
    for doc_id, score in result.hits:
        print(f"{doc_id} {score}")

def build_parser() -> argparse.ArgumentParser:
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
        choices=profile_names("openalex"),
        help="OpenAlex profile to use (see project-config.toml for filter conditions). "
             "Profiles with materialize = false are only counted, not copied."
    )
    gen_works_subset_parser.set_defaults(func=cmd_gen_works_subset)

    # build_posting
    build_posting_parser = subparsers.add_parser("build-posting", help="Build inverted index posting from data.")
    build_posting_parser.add_argument(
        "--profile",
        required=True,
        choices=profile_names(),
        help="Profile to build (see project-config.toml). Copied OpenAlex profiles need gen-works-subset first."
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
        choices=profile_names(),
        help="Profile to query (Defaults to full-en)."
    )
    query_parser.set_defaults(func=cmd_query)

    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
