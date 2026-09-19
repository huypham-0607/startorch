import argparse
import resource

import pandas as pd

from pathlib import Path
from startorch import PostingBuilder, Searcher, get_logger, profile, read_query_file

logger = get_logger(__name__)

QUERY_SET_LEN = 2000

def write_perf_parquet(
    run_name: str,
    query_latency: list[float],
    engine_latency: float,
    max_rss: int,
    out_path: Path,
):
    row = pd.DataFrame([{
        "run_name": run_name,
        "query_latency": query_latency,
        "engine_latency": engine_latency,
        "max_rss": max_rss,
    }])

    out_path.parent.mkdir(parents=True, exist_ok=True)

    if out_path.exists():
        existing = pd.read_parquet(out_path)
        combined = pd.concat([existing, row], ignore_index=True)
    else:
        combined = row

    combined.to_parquet(out_path)

def run_queries(
    profile_name: str,
    filename: str,
    k: int,
    engine: str = "bmw",
    cap: int | None = None,
) -> tuple:
    """Run one query set against one profile's index.

    Returns (per-query latency in ms, index load latency in ms, peak RSS in KB).
    Latency is the C++ search time only, as before, so results compare with
    earlier runs.
    """
    p = profile(profile_name)
    if p.kind == "msmarco" and not p.meta_path.is_file():
        PostingBuilder(p).build()

    query_path = p.query_dir / filename
    logger.info(f"Started running {profile_name} queries from {query_path}, k = {k}, engine = {engine}, cap = {cap}.")

    queries = [text for _, text in read_query_file(query_path, cap)]
    searcher = Searcher(p)
    results = searcher.search_many(queries, k, exhaustive=(engine == "exhaustive"))

    logger.info(f"Finished running {profile_name} queries from {query_path}, k = {k}, engine = {engine}.")

    query_latency_ms = [r.elapsed.total_seconds() * 1000 for r in results]
    engine_latency_ms = searcher.load_latency.total_seconds() * 1000

    # Assuming we run on Linux, max_rss is in kilobytes
    max_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

    return query_latency_ms, engine_latency_ms, max_rss

def main():
    parser = argparse.ArgumentParser(
        description="Run one BM25 performance benchmark and append its result to perf_raw.parquet. "
        "Invoke this once per (query file, k) combination (e.g. from a shell loop) rather than "
        "looping in-process, so each run gets its own OS process and therefore its own "
        "independent peak-RSS reading."
    )
    parser.add_argument(
        "type", type=int, choices=[0, 1],
        help="Corpus type: 0 = msmarco, 1 = full-en"
    )
    parser.add_argument(
        "filename", type=str,
        help="Query file name; the containing directory is resolved internally based on type"
    )
    parser.add_argument(
        "--k", type=int, default=None,
        help="Top-k depth to query at. Required when type=1 (full-en) - each k needs its own "
        "process invocation for RAM isolation. Ignored when type=0 (msmarco), which is fixed at k=1000."
    )
    parser.add_argument(
        "--engine", choices=["bmw", "exhaustive"], default="bmw",
        help="Query engine variant to benchmark. Only applies when type=1 (full-en) - "
        "'bmw' uses the pruned WAND/BMW query path, 'exhaustive' scores every candidate "
        "document with no pruning. Ignored when type=0 (msmarco)."
    )
    parser.add_argument(
        "--cap", type=int, default=QUERY_SET_LEN,
        help="Max number of queries to use from the query set. Only applies when type=1 "
        "(full-en). Useful for keeping --engine=exhaustive runs tractable, since it has no "
        "pruning to speed it up. Ignored when type=0 (msmarco)."
    )
    args = parser.parse_args()

    if args.type == 1 and args.k is None:
        parser.error("--k is required when type=1 (full-en)")

    if args.type == 1:
        out_path = profile("full-en").result_dir / "perf_raw.parquet"

        logger.info(f"Current run: k = {args.k}, engine = {args.engine}, cap = {args.cap}...")
        query_latency, engine_latency, max_rss = run_queries("full-en", args.filename, args.k, args.engine, args.cap)
        logger.info(f"Writing full-en performance data to {out_path}, k = {args.k}, engine = {args.engine}.")
        file_name = f"{args.filename}_{args.k}"
        if args.engine == "exhaustive":
            file_name += f"_{args.engine}"
        write_perf_parquet(
            file_name, query_latency, engine_latency, max_rss, out_path
        )

    else:
        # MS MARCO: pruned search at k = 1000 over the whole query file, as before.
        query_latency, engine_latency, max_rss = run_queries("msmarco", args.filename, 1000)

        out_path = profile("msmarco").result_dir / "perf_raw.parquet"

        logger.info(f"Writing MS MARCO performance data to {out_path}.")
        write_perf_parquet(args.filename, query_latency, engine_latency, max_rss, out_path)

if __name__ == "__main__":
    main()
