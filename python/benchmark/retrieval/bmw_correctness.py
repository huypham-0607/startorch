import argparse
import csv

import pandas as pd

from pathlib import Path
from startorch import PostingBuilder, Searcher, get_logger, profile, read_query_file

logger = get_logger(__name__)

# Each result list is Searcher hits: (raw_id, score) pairs, best first.
# For MS MARCO the raw id is the passage id (pid).

def write_ms(qid_list: list, results: list[list[tuple[int, float]]], out_path: Path):
    buffer = []
    for qid, ranked in zip(qid_list, results):
        for rank, (pid, score) in enumerate(ranked, start=1):
            buffer.append([qid, pid, rank])

    with open(out_path, "w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f, delimiter="\t")
        writer.writerows(buffer)

def write_trec(qid_list: list, results: list[list[tuple[int, float]]], out_path: Path):
    buffer = []
    for qid, ranked in zip(qid_list, results):
        for rank, (pid, score) in enumerate(ranked, start=1):
            buffer.append([qid, "Q0", pid, rank, score, "BMW"])

    with open(out_path, "w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f, delimiter="\t")
        writer.writerows(buffer)

def write_correctness_parquet(
    run_name: str,
    rank_matches: list[bool],
    query_matches: int,
    missing_docids: list[list[int]],
    extra_docids: list[list[int]],
    bmw_min_score: list[float],
    bmw_max_score: list[float],
    exhaustive_min_score: list[float],
    exhaustive_max_score: list[float],
    out_path: Path,
):
    row = pd.DataFrame([{
        "run_name": run_name,
        "rank_matches": rank_matches,
        "query_matches": query_matches,
        "missing_docids": missing_docids,
        "extra_docids": extra_docids,
        "bmw_min_score": bmw_min_score,
        "bmw_max_score": bmw_max_score,
        "exhaustive_min_score": exhaustive_min_score,
        "exhaustive_max_score": exhaustive_max_score,
    }])

    out_path.parent.mkdir(parents=True, exist_ok=True)

    if out_path.exists():
        existing = pd.read_parquet(out_path)
        combined = pd.concat([existing, row], ignore_index=True)
    else:
        combined = row

    combined.to_parquet(out_path)

def ms_marco_benchmark():
    p = profile("msmarco")
    if not p.meta_path.is_file():
        PostingBuilder(p).build()

    queries = read_query_file(p.query_dir / "queries.dev.small.tsv")
    searcher = Searcher(p)
    qid_list = [qid for qid, _ in queries]
    results = [searcher.search(text, 1000).hits for _, text in queries]

    ms_path = p.result_dir / "ms_mrr10.tsv"
    trec_path = p.result_dir / "trec_recall1000.trec"
    p.result_dir.mkdir(parents=True, exist_ok=True)

    write_ms(qid_list, results, ms_path)
    write_trec(qid_list, results, trec_path)

def compare_bmw_exhaustive(queryset: str, k: int, x: int) -> tuple:
    """Compare the first x queries of queryset between the pruned BMW engine
    and the unpruned exhaustive engine, both run at the same k.

    Returns (rank_matches, query_matches, missing_docids,
    extra_docids, bmw_min_score, bmw_max_score, exhaustive_min_score,
    exhaustive_max_score) - all but query_matches are lists with
    one entry per query:
      rank_matches: bool - True if BMW's full ranked result list
        (doc_id and score at every position) is identical to exhaustive's
        for that query.
      query_matches: int, sum of rank_matches - how many of
        the x queries matched exactly.
      missing_docids: list[int] - OpenAlex ids in exhaustive's result but absent
        from BMW's. False negatives - the real bug class, since it means
        BMW's pruning threw away a document it should have kept.
      extra_docids: list[int] - OpenAlex ids in BMW's result but absent from
        exhaustive's. Should be impossible given exhaustive is a full,
        unpruned scan - a non-empty entry here points at a scoring or
        heap bug rather than an over-aggressive pruning bug.
      bmw_min_score / bmw_max_score: float or None - min/max score in
        BMW's result list for that query (None if it returned no results).
      exhaustive_min_score / exhaustive_max_score: same, for exhaustive's
        result list.
    """
    p = profile("full-en")
    queries = [text for _, text in read_query_file(p.query_dir / queryset, cap=x)]

    # One engine serves both passes, so the index loads once.
    searcher = Searcher(p)
    logger.info(f"Running BMW engine on first {x} queries of {queryset}, k = {k}...")
    bmw_results = [r.hits for r in searcher.search_many(queries, k)]
    logger.info(f"Running exhaustive engine on first {x} queries of {queryset}, k = {k}...")
    exhaustive_results = [r.hits for r in searcher.search_many(queries, k, exhaustive=True)]

    rank_matches = []
    missing_docids = []
    extra_docids = []
    bmw_min_score = []
    bmw_max_score = []
    exhaustive_min_score = []
    exhaustive_max_score = []

    for bmw_res, exhaustive_res in zip(bmw_results, exhaustive_results):
        rank_matches.append(bmw_res == exhaustive_res)

        bmw_docs = {doc_id for doc_id, _ in bmw_res}
        exhaustive_docs = {doc_id for doc_id, _ in exhaustive_res}
        missing_docids.append(list(exhaustive_docs - bmw_docs))
        extra_docids.append(list(bmw_docs - exhaustive_docs))

        bmw_scores = [score for _, score in bmw_res]
        exhaustive_scores = [score for _, score in exhaustive_res]
        bmw_min_score.append(min(bmw_scores) if bmw_scores else None)
        bmw_max_score.append(max(bmw_scores) if bmw_scores else None)
        exhaustive_min_score.append(min(exhaustive_scores) if exhaustive_scores else None)
        exhaustive_max_score.append(max(exhaustive_scores) if exhaustive_scores else None)

    query_matches = sum(rank_matches)

    return (
        rank_matches, query_matches,
        missing_docids, extra_docids,
        bmw_min_score, bmw_max_score,
        exhaustive_min_score, exhaustive_max_score,
    )

def main():
    parser = argparse.ArgumentParser(
        description="Run either the MS MARCO correctness benchmark or the full-en BMW-vs-exhaustive check."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("ms-marco", help="Run BMW against MS MARCO dev-small and write ms_mrr10.tsv/trec_recall1000.trec.")

    full_en_parser = subparsers.add_parser(
        "full-en-check",
        help="Compare BMW vs exhaustive results on a full-en query set and append to correctness_raw.parquet."
    )
    full_en_parser.add_argument("queryset", type=str, help="Query set file name (e.g. random_set.tsv)")
    full_en_parser.add_argument("--k", type=int, required=True, help="Top-k depth to compare at")
    full_en_parser.add_argument("--x", type=int, required=True, help="Compare the first x queries of the query set")

    args = parser.parse_args()

    if args.command == "ms-marco":
        ms_marco_benchmark()
    else:
        logger.info(f"Comparing BMW vs exhaustive on {args.queryset}, k = {args.k}, first {args.x} queries...")
        (
            rank_matches, query_matches,
            missing_docids, extra_docids,
            bmw_min_score, bmw_max_score,
            exhaustive_min_score, exhaustive_max_score,
        ) = compare_bmw_exhaustive(args.queryset, args.k, args.x)

        out_path = profile("full-en").result_dir / "correctness_raw.parquet"

        logger.info(f"Writing correctness data to {out_path}.")
        write_correctness_parquet(
            f"{args.queryset}_{args.k}_{args.x}",
            rank_matches, query_matches,
            missing_docids, extra_docids,
            bmw_min_score, bmw_max_score,
            exhaustive_min_score, exhaustive_max_score,
            out_path
        )

if __name__ == "__main__":
    main()
