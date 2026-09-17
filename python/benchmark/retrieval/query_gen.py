import csv
import random

import numpy as np
import pandas as pd

from pathlib import Path
from startorch import get_logger, load_config, load_benchmark_config, startorch_cpp
from ms_marco_pipeline import build_posting, run_queries_perf_metrics

logger = get_logger(__name__)

QUERY_RESULT_FOLDER = "query_result"

SEED = 67
N_QUERIES = 2000
N_WEIGHTED_QUERIES = 5000
QUERY_LEN_RANGE = (5, 10)

def _percentile_slice(
    mapping_sorted: list[tuple[str, int]],
    fraction: float,
    from_end: bool = False,
) -> list[tuple[str, int]]:
    """Slice mapping_sorted (descending by df) into a df percentile band.

    from_end=False takes the highest-df fraction (the front of the list);
    from_end=True takes the lowest-df fraction (the back of the list).
    """
    n = len(mapping_sorted)
    k = int(n * fraction)
    if from_end:
        return mapping_sorted[n - k:]
    return mapping_sorted[:k]

def _write_query_set(
    term_pool: list[tuple[str, int]],
    out_path: Path,
    n_queries: int = N_QUERIES,
    seed: int = SEED,
    query_len_range: tuple[int, int] = QUERY_LEN_RANGE,
):
    """Write n_queries deterministic queries sampled from term_pool to out_path.

    Each query draws a random length in query_len_range and that many
    distinct terms from term_pool (without replacement within a query,
    with replacement across queries).
    """
    rng = random.Random(seed)
    terms = [term for term, _ in term_pool]

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f, delimiter="\t")
        for qid in range(n_queries):
            query_len = rng.randint(*query_len_range)
            query_terms = rng.sample(terms, query_len)
            writer.writerow([qid, " ".join(query_terms)])

def generate_random_set(mapping_sorted: list[tuple[str, int]], out_dir: Path):
    """random_set.tsv: uniform random sampling across the entire vocabulary."""
    logger.info("Generating random_set...")
    _write_query_set(mapping_sorted, out_dir / "random_set.tsv")
    logger.info("Finished generating random_set.")

def generate_common_set(mapping_sorted: list[tuple[str, int]], out_dir: Path):
    """common_set.tsv: terms drawn from the top 1% highest-df band."""
    logger.info("Generating common_set...")
    pool = _percentile_slice(mapping_sorted, 0.01)
    _write_query_set(pool, out_dir / "common_set.tsv")
    logger.info("Finished generating common_set.")

def generate_very_common_set(mapping_sorted: list[tuple[str, int]], out_dir: Path):
    """very_common_set.tsv: terms drawn from the top 0.01% highest-df band."""
    logger.info("Generating very_common_set...")
    pool = _percentile_slice(mapping_sorted, 0.0001)
    _write_query_set(pool, out_dir / "very_common_set.tsv")
    logger.info("Finished generating very_common_set.")

def generate_rare_set(mapping_sorted: list[tuple[str, int]], out_dir: Path):
    """rare_set.tsv: terms drawn from the bottom 10% lowest-df band."""
    logger.info("Generating rare_set...")
    pool = _percentile_slice(mapping_sorted, 0.1, from_end=True)
    _write_query_set(pool, out_dir / "rare_set.tsv")
    logger.info("Finished generating rare_set.")

def generate_skewed_set(mapping_sorted: list[tuple[str, int]], out_dir: Path):
    """skewed_set.tsv: terms drawn from the union of the top 0.01% and bottom 0.01% df band."""
    logger.info("Generating skewed_set...")
    pool = (
        _percentile_slice(mapping_sorted, 0.0001)
        + _percentile_slice(mapping_sorted, 0.0001, from_end=True)
    )
    _write_query_set(pool, out_dir / "skewed_set.tsv")
    logger.info("Finished generating skewed_set.")

def _sample_weighted_distinct(
    terms: list[str],
    prefix: np.ndarray,
    total: int,
    k: int,
    rng: np.random.Generator,
) -> list[str]:
    """Draw k distinct terms, each with probability proportional to its df."""
    picked: set[int] = set()
    while len(picked) < k:
        needed = k - len(picked)
        draws = rng.integers(0, total, size=needed)
        indices = np.searchsorted(prefix, draws, side="right")
        picked.update(indices.tolist())
    return [terms[i] for i in picked]

def generate_weighted_set(mapping_sorted: list[tuple[str, int]], out_dir: Path):
    """weighted_set.tsv: each term has a df / sum(df) chance of appearing."""
    logger.info("Generating weighted_set...")

    terms = [term for term, _ in mapping_sorted]
    dfs = np.array([df for _, df in mapping_sorted], dtype=np.int64)
    prefix = np.cumsum(dfs)
    total = int(prefix[-1])

    np_rng = np.random.default_rng(SEED)
    py_rng = random.Random(SEED)

    out_path = out_dir / "weighted_set.tsv"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f, delimiter="\t")
        for qid in range(N_WEIGHTED_QUERIES):
            query_len = py_rng.randint(*QUERY_LEN_RANGE)
            query_terms = _sample_weighted_distinct(terms, prefix, total, query_len, np_rng)
            writer.writerow([qid, " ".join(query_terms)])

    logger.info("Finished generating weighted_set.")

N_QUERIES_SHORT = 2000
QUERY_LEN_RANGE_SHORT = (3, 4)

def generate_short_random_set(mapping_sorted: list[tuple[str, int]], out_dir: Path):
    """short_random_set.tsv: uniform random sampling across the entire vocabulary."""
    logger.info("Generating short_random_set...")
    _write_query_set(
        mapping_sorted, out_dir / "short_random_set.tsv",
        n_queries=N_QUERIES_SHORT, query_len_range=QUERY_LEN_RANGE_SHORT
    )
    logger.info("Finished generating short_random_set.")

def generate_short_common_set(mapping_sorted: list[tuple[str, int]], out_dir: Path):
    """short_common_set.tsv: terms drawn from the top 1% highest-df band."""
    logger.info("Generating short_common_set...")
    pool = _percentile_slice(mapping_sorted, 0.01)
    _write_query_set(
        pool, out_dir / "short_common_set.tsv",
        n_queries=N_QUERIES_SHORT, query_len_range=QUERY_LEN_RANGE_SHORT
    )
    logger.info("Finished generating short_common_set.")

def generate_short_very_common_set(mapping_sorted: list[tuple[str, int]], out_dir: Path):
    """short_very_common_set.tsv: terms drawn from the top 0.01% highest-df band."""
    logger.info("Generating short_very_common_set...")
    pool = _percentile_slice(mapping_sorted, 0.0001)
    _write_query_set(
        pool, out_dir / "short_very_common_set.tsv",
        n_queries=N_QUERIES_SHORT, query_len_range=QUERY_LEN_RANGE_SHORT
    )
    logger.info("Finished generating short_very_common_set.")

def generate_short_rare_set(mapping_sorted: list[tuple[str, int]], out_dir: Path):
    """short_rare_set.tsv: terms drawn from the bottom 10% lowest-df band."""
    logger.info("Generating short_rare_set...")
    pool = _percentile_slice(mapping_sorted, 0.1, from_end=True)
    _write_query_set(
        pool, out_dir / "short_rare_set.tsv",
        n_queries=N_QUERIES_SHORT, query_len_range=QUERY_LEN_RANGE_SHORT
    )
    logger.info("Finished generating short_rare_set.")

def generate_short_skewed_set(mapping_sorted: list[tuple[str, int]], out_dir: Path):
    """short_skewed_set.tsv: terms drawn from the union of the top 0.01% and bottom 0.01% df band."""
    logger.info("Generating short_skewed_set...")
    pool = (
        _percentile_slice(mapping_sorted, 0.0001)
        + _percentile_slice(mapping_sorted, 0.0001, from_end=True)
    )
    _write_query_set(
        pool, out_dir / "short_skewed_set.tsv",
        n_queries=N_QUERIES_SHORT, query_len_range=QUERY_LEN_RANGE_SHORT
    )
    logger.info("Finished generating short_skewed_set.")

def main():
    """Produced a few query set for full-en based on term occurences

    Each query set will have 2000 queries, each with length (number of terms) of [5,10].
    
    Query sets will be saved in tsv. Format of all query sets are:
        qid query_string

    List of query sets
        - random_set.tsv: Uniformed random sampling.
        - common_set.tsv: Random terms with top 1% highest df.
        - very_common_set.tsv: Random terms with top 0.01% df.
        - rare_set.tsv: Random terms with bottom 10% lowest df.
        - skewed_set.tsv: Random terms with top 0.01% highest df or top 0.01% lowest df.
        - weighted_set.tsv: Random terms with (df / sum(df)) chance of appearing..
    """
    config = load_config()
    benchmark_config = load_benchmark_config()

    paths = config["data-path"]

    full_meta_path = Path(paths["posting-path"]) / "full-en" / config["posting"]["posting-folder"] / "metadata.bin"
    query_data_dir = Path(benchmark_config["paths"]["data-dir"]) / "full-en"
    query_data_dir.mkdir(parents=True, exist_ok=True)

    # flag = True
    # query_sets = ["random_set.tsv", "common_set.tsv", "very_common_set.tsv", "rare_set.tsv", "skewed_set.tsv"]
    # for query_set in query_sets:
    #     flag = flag and (query_data_dir / query_set).is_file()

    # if flag:
    #     logger.info(f"All {len(query_sets)} sets already present, skipping...")
    #     return

    mapping = startorch_cpp.read_term_df_mapping(full_meta_path)
    mapping_sorted = sorted(mapping, key=lambda term_df: term_df[1], reverse=True)

    logger.info(f"Total no of terms: {len(mapping_sorted)}.")

    generate_random_set(mapping_sorted, query_data_dir)
    generate_common_set(mapping_sorted, query_data_dir)
    generate_very_common_set(mapping_sorted, query_data_dir)
    generate_rare_set(mapping_sorted, query_data_dir)
    generate_skewed_set(mapping_sorted, query_data_dir)
    generate_weighted_set(mapping_sorted, query_data_dir)

    generate_short_random_set(mapping_sorted, query_data_dir)
    generate_short_common_set(mapping_sorted, query_data_dir)
    generate_short_very_common_set(mapping_sorted, query_data_dir)
    generate_short_rare_set(mapping_sorted, query_data_dir)
    generate_short_skewed_set(mapping_sorted, query_data_dir)

if __name__ == "__main__":
    main()

