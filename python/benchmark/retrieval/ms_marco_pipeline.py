import io
import struct
import duckdb as db
import tomllib
import csv
import time

from datetime import timedelta
from pathlib import Path
from startorch import get_logger, load_benchmark_config, Tokenizer, startorch_cpp, PROJECT_ROOT

logger = get_logger(__name__)

TOKEN_STREAM_FOLDER = "token_stream"
POSTING_FOLDER = "posting"
PARTIAL_FOLDER = "posting/partial"
QUERY_RESULT_FOLDER = "query_result"

MEM_LIMIT = (1<<30)
K1 = 0.82
B = 0.68
BLOCK_SIZE = 128
SPLIT_SIZE = (1<<30)

class TokenizerMSMARCO:
    def __init__(
        self
    ):
        self.stop_word_list = r"\b(i|me|my|myself|we|our|ours|ourselves|you|your|yours|yourself|yourselves|he|him|his|himself|she|her|hers|herself|it|its|itself|they|them|their|theirs|themselves|what|which|who|whom|this|that|these|those|am|is|are|was|were|be|been|being|have|has|had|having|do|does|did|doing|a|an|the|and|but|if|or|because|as|until|while|of|at|by|for|with|about|against|between|into|through|during|before|after|above|below|to|from|up|down|in|out|on|off|over|under|again|further|then|once|here|there|when|where|why|how|all|any|both|each|few|more|most|other|some|such|no|nor|not|only|own|same|so|than|too|very|s|t|can|will|just|don|should|now)\b"

    @staticmethod
    def get_token_stream_file_name(idx: int):
        return f"token_{idx:04d}.bin"

    def get_token(
        self,
        db_path: Path,
        out_path: Path,
        spill_path: Path,
        row_per_chunk: int = (1<<18),
        chunk_per_file: int = (1<<8)
    ):
        con = db.connect(config={"temp_directory": str(spill_path)})

        logger.info(f"Start serializing corpus from {db_path}.")

        tokenized = con.sql(f"""
            INSTALL fts;
            LOAD fts;
            SELECT
                pid AS id,
                list_transform(
                    regexp_extract_all(
                        regexp_replace(
                            lower(concat_ws(' ',
                                coalesce(passage, '')
                            )),
                            '{self.stop_word_list}',
                            ' ',
                            'g'
                        ),
                        '[a-z0-9]+'
                    ),
                    token -> stem(token,'english')
                ) AS tokens
            FROM read_csv('{db_path}/collection.tsv', header=False, sep="\t", names=['pid','passage'])
        """)

        logger.info(f"Creating _token_stream table...")

        rel = con.sql(""" SELECT id, tokens FROM tokenized ORDER BY id ASC """)
        con.sql(""" CREATE OR REPLACE TEMP TABLE _token_stream AS SELECT id, unnest(tokens) AS token FROM rel """)

        logger.info(f"Finished creating _token_stream table.")

        token_stream = con.sql("SELECT id, token FROM _token_stream")

        buf = io.BytesIO()

        file_count = 0

        no_tokens = 0
        max_token_length = 0
        
        logger.info(f"Begin serializing token to {str(out_path)}.")

        batch = token_stream.fetchmany(row_per_chunk)
        out_path.mkdir(parents=True, exist_ok=True)
        while batch:
            chunk_count = 0
            file_name = self.get_token_stream_file_name(file_count)
            logger.info(f"Outputting stream into file {file_name}.")

            with open(f"{str(out_path)}/{file_name}", "wb") as out_file:
                while batch and chunk_count < chunk_per_file:
                    for doc_id, token in batch:
                        # Convert to bytes object
                        encoded = token.encode("utf-8")

                        no_tokens += 1
                        max_token_length = max(max_token_length, len(encoded))

                        # Convert into little endian signed ll, usigned short
                        buf.write(struct.pack('<qH', doc_id, len(encoded)))
                        buf.write(encoded)

                    out_file.write(buf.getvalue())

                    buf.seek(0); buf.truncate()
                    chunk_count += 1
                    batch = token_stream.fetchmany(row_per_chunk)

            file_count += 1
        logger.info(f"Finished serializing corpus from {db_path}.")
        logger.info(f"No of tokens: {no_tokens}")
        logger.info(f"Max token length = {max_token_length}")


def build_posting(
    is_forced: bool = False
):
    benchmark_config = load_benchmark_config()
    Path(benchmark_config["paths"]["benchmark-dir"]).mkdir(parents=True, exist_ok=True)

    data_dir = Path(benchmark_config["msmarco"]["data-dir"])
    token_stream_dir = Path(benchmark_config["msmarco"]["posting-dir"]) / TOKEN_STREAM_FOLDER
    spill_dir = Path(benchmark_config["paths"]["spill-dir"])
    query_result_dir = Path(benchmark_config["msmarco"]["posting-dir"]) / QUERY_RESULT_FOLDER
    posting_dir = Path(benchmark_config["msmarco"]["posting-dir"]) / POSTING_FOLDER
    partial_dir = Path(benchmark_config["msmarco"]["posting-dir"]) / PARTIAL_FOLDER
    
    if not is_forced and (posting_dir / startorch_cpp.file_names.METADATA_BIN).is_file():
        return

    data_dir.mkdir(parents=True, exist_ok=True)
    token_stream_dir.mkdir(parents=True, exist_ok=True)
    spill_dir.mkdir(parents=True, exist_ok=True)
    posting_dir.mkdir(parents=True, exist_ok=True)
    partial_dir.mkdir(parents=True, exist_ok=True)

    tokenizer = TokenizerMSMARCO()
    
    tokenizer.get_token(data_dir, token_stream_dir, spill_dir)

    startorch_cpp.build_doc_len(token_stream_dir, posting_dir)
    startorch_cpp.build_inverted_blocks(
        token_stream_dir,
        partial_dir,
        MEM_LIMIT
    )
    startorch_cpp.merge_inverted_blocks(
        partial_dir,
        posting_dir,
        K1,
        B,
        BLOCK_SIZE,
        SPLIT_SIZE
    )

def run_queries(query_path: Path) -> tuple:
    benchmark_config = load_benchmark_config()
    Path(benchmark_config["paths"]["benchmark-dir"]).mkdir(parents=True, exist_ok=True)

    posting_dir = Path(benchmark_config["msmarco"]["posting-dir"]) / POSTING_FOLDER
    meta_path = posting_dir / startorch_cpp.file_names.METADATA_BIN

    with open(query_path, mode='r', encoding='utf-8') as file:
        queries = list(csv.reader(file, delimiter='\t'))

    tokenizer = Tokenizer()

    query_list = list(tokenizer.tokenize_query(query) for _, query in queries)
    pid_list = list(pid for pid, _ in queries)

    engine = startorch_cpp.QueryEngine(meta_path)
    results = [engine.query(terms, 1000)[0] for terms in query_list]

    return pid_list, results

def run_queries_perf_metrics(query_path: Path) -> tuple:
    benchmark_config = load_benchmark_config()
    Path(benchmark_config["paths"]["benchmark-dir"]).mkdir(parents=True, exist_ok=True)

    posting_dir = Path(benchmark_config["msmarco"]["posting-dir"]) / POSTING_FOLDER
    meta_path = posting_dir / startorch_cpp.file_names.METADATA_BIN

    logger.info(f"Reading queries from {query_path}...")

    with open(query_path, mode='r', encoding='utf-8') as file:
        queries = list(csv.reader(file, delimiter='\t'))

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
    query_latency = [engine.query(terms, 1000)[1] for terms in query_list]
    logger.info(f"Engine finished running, returning benchmarked results.")

    return engine_latency, query_latency