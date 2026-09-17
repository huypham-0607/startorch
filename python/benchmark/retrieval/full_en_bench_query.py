import io
import struct
import duckdb as db
import tomllib
import csv
import time

from datetime import timedelta
from pathlib import Path
from startorch import get_logger, load_benchmark_config, load_config, Tokenizer, startorch_cpp, PROJECT_ROOT

logger = get_logger(__name__)

TOKEN_STREAM_FOLDER = "token_stream"
POSTING_FOLDER = "posting"
PARTIAL_FOLDER = "posting/partial"
QUERY_RESULT_FOLDER = "query_result"

QUERY_SET_LEN = 2000


def run_queries_perf_metrics(query_path: Path, k: int, cap: int = QUERY_SET_LEN) -> tuple:
    benchmark_config = load_benchmark_config()
    config = load_config()

    posting_dir = Path(config["data-path"]["posting-path"]) / "full-en" / POSTING_FOLDER
    meta_path = posting_dir / startorch_cpp.file_names.METADATA_BIN

    logger.info(f"Reading queries from {query_path}...")

    with open(query_path, mode='r', encoding='utf-8') as file:
        queries = list(csv.reader(file, delimiter='\t'))[:cap]

    logger.info(f"Finished reading queries from {query_path}...")

    tokenizer = Tokenizer()

    logger.info(f"Tokenizing queries...")
    query_list = list(tokenizer.tokenize_query(query) for _, query in queries)
    logger.info(f"Finished tokenizing queries.")

    logger.info(f"Loading index from {meta_path}...")
    start = time.perf_counter()
    engine = startorch_cpp.QueryEngine(meta_path)
    engine_latency = timedelta(seconds=time.perf_counter() - start)

    logger.info(f"Passing queries to BMW engine. Running engine...")
    results, query_latency = [], []
    for terms in query_list:
        result, elapsed = engine.query(terms, k)
        results.append(result)
        query_latency.append(elapsed)
    logger.info(f"Engine finished running, returning benchmarked results.")

    return results, engine_latency, query_latency


def run_queries_exhaustive_perf_metrics(query_path: Path, k: int, cap: int = QUERY_SET_LEN) -> tuple:
    benchmark_config = load_benchmark_config()
    config = load_config()

    posting_dir = Path(config["data-path"]["posting-path"]) / "full-en" / POSTING_FOLDER
    meta_path = posting_dir / startorch_cpp.file_names.METADATA_BIN

    logger.info(f"Reading queries from {query_path}...")

    with open(query_path, mode='r', encoding='utf-8') as file:
        queries = list(csv.reader(file, delimiter='\t'))[:cap]

    logger.info(f"Finished reading queries from {query_path}...")

    tokenizer = Tokenizer()

    logger.info(f"Tokenizing queries...")
    query_list = list(tokenizer.tokenize_query(query) for _, query in queries)
    logger.info(f"Finished tokenizing queries.")

    logger.info(f"Loading index from {meta_path}...")
    start = time.perf_counter()
    engine = startorch_cpp.QueryEngine(meta_path)
    engine_latency = timedelta(seconds=time.perf_counter() - start)

    logger.info(f"Passing queries to Exhaustive engine. Running engine...")
    results, query_latency = [], []
    for terms in query_list:
        result, elapsed = engine.query_exhaustive(terms, k)
        results.append(result)
        query_latency.append(elapsed)
    logger.info(f"Engine finished running, returning benchmarked results.")

    return results, engine_latency, query_latency