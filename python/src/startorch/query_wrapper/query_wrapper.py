"""Tokenize input query, pass to C++ retrieval engine, and return top K results.

"""
import struct
import duckdb as db

from pathlib import Path
from startorch import get_logger, Tokenizer, startorch_cpp

def retrieve(
    query_string: str,
    k: int,
    root_path: Path,
    posting_folder: str,
    lookup_folder: str,
    lookup_file_name: str,
):
    tokenizer = Tokenizer()

    terms = tokenizer.tokenize_query(query_string)
    meta_path = root_path / posting_folder / startorch_cpp.file_names.METADATA_BIN
    engine = startorch_cpp.QueryEngine(meta_path)
    results, elapsed = engine.query(terms, k)

    lookup_path = root_path / lookup_folder / lookup_file_name

    with open(lookup_path, "rb") as f:
        for i in range (0, len(results)):
            f.seek(results[i][1] * 12)
            chunk = f.read(8)
            results[i] = (results[i][0], struct.unpack('<q', chunk)[0])

    for score, doc_id in results:
        print(f"{doc_id} {score}")