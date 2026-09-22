"""Tokenize documents into the binary token stream the C++ index build reads.

Every corpus goes through the same tokenizer. A source query supplies
(raw_id BIGINT, text VARCHAR) rows; see openalex_source, msmarco_source and
profile_source. Documents and queries share one token expression, so they are
tokenized identically.

Attributes:
    STOP_WORDS: Regex of English stopwords, removed before splitting into tokens.
"""

import threading

import duckdb as db
import numpy as np
import pyarrow as pa
import pyarrow.compute

from pathlib import Path
from typing import Any
from startorch.utils.duckdb import fetch_one
from startorch.utils.logger import get_logger
from startorch.lexical.doc_id_lookup import lookup_records, map_raw_ids
from startorch.utils.paths import Profile, corpus_paths

logger = get_logger(__name__)

# pyarrow.compute creates its functions at import time, so type checkers can't
# see them; treat the module as untyped rather than silencing each call.
pc: Any = pyarrow.compute

STOP_WORDS = r"\b(i|me|my|myself|we|our|ours|ourselves|you|your|yours|yourself|yourselves|he|him|his|himself|she|her|hers|herself|it|its|itself|they|them|their|theirs|themselves|what|which|who|whom|this|that|these|those|am|is|are|was|were|be|been|being|have|has|had|having|do|does|did|doing|a|an|the|and|but|if|or|because|as|until|while|of|at|by|for|with|about|against|between|into|through|during|before|after|above|below|to|from|up|down|in|out|on|off|over|under|again|further|then|once|here|there|when|where|why|how|all|any|both|each|few|more|most|other|some|such|no|nor|not|only|own|same|so|than|too|very|s|t|can|will|just|don|should|now)\b"


def token_expression(text_sql: str) -> str:
    """Returns a DuckDB expression that turns a text value into its token list.

    The expression lowercases, strips stopwords, splits into [a-z0-9]+ words,
    and applies DuckDB's English stemmer.

    Args:
        text_sql: A SQL expression or parameter yielding the text, e.g. "text"
            or "$query".

    Returns:
        A SQL expression producing a list of tokens.
    """
    return f"""list_transform(
        regexp_extract_all(
            regexp_replace(lower({text_sql}), '{STOP_WORDS}', ' ', 'g'),
            '[a-z0-9]+'
        ),
        token -> stem(token, 'english')
    )"""


def openalex_source(parquet_glob: str, condition: str | None = None) -> str:
    """Returns a source query over OpenAlex Works parquet files.

    Each row is the numeric OpenAlex id and one text field: the title, the topic
    hierarchy (topic, subfield, field, domain) and the keywords.

    Args:
        parquet_glob: Glob of Works parquet files, from a subset or the full corpus.
        condition: Optional SQL condition selecting which works to include.

    Returns:
        A SQL query yielding (raw_id BIGINT, text VARCHAR) rows.
    """
    where = f"WHERE {condition}" if condition else ""
    return f"""
        SELECT
            regexp_replace(id, 'W', '')::BIGINT AS raw_id,
            concat_ws(' ',
                coalesce(title, ''),
                list_aggregate(list_transform(topics, t -> t.display_name), 'string_agg', ' '),
                list_aggregate(list_transform(topics, t -> t.subfield_display_name), 'string_agg', ' '),
                list_aggregate(list_transform(topics, t -> t.field_display_name), 'string_agg', ' '),
                list_aggregate(list_transform(topics, t -> t.domain_display_name), 'string_agg', ' '),
                list_aggregate(list_transform(keywords, k -> k.display_name), 'string_agg', ' ')
            ) AS text
        FROM read_parquet('{parquet_glob}')
        {where}
    """


def msmarco_source(collection: Path) -> str:
    """Returns a source query over the MS MARCO passage collection.

    Args:
        collection: Path to collection.tsv, one (pid, passage) row per line.

    Returns:
        A SQL query yielding (raw_id BIGINT, text VARCHAR) rows.
    """
    return f"""
        SELECT pid::BIGINT AS raw_id, coalesce(passage, '') AS text
        FROM read_csv('{collection}', header=False, sep='\t', names=['pid', 'passage'])
    """


def profile_source(profile: Profile) -> str:
    """Returns the source query for a profile's documents.

    MS MARCO reads its collection. An OpenAlex profile reads its copied subset if
    it is materialized, and otherwise the full corpus through its filter.

    Args:
        profile: The profile whose documents to tokenize.

    Returns:
        A SQL query yielding (raw_id BIGINT, text VARCHAR) rows.
    """
    if profile.kind == "msmarco":
        assert profile.collection is not None, f"MS MARCO profile {profile.name} has no collection"
        return msmarco_source(profile.collection)
    assert profile.subset_dir is not None and profile.filter is not None, f"{profile.name} is not an OpenAlex profile"
    if profile.materialize:
        return openalex_source(str(profile.subset_dir / "**" / "*.parquet"))
    return openalex_source(corpus_paths().works_glob(), profile.filter)


class Tokenizer:
    """Tokenizes documents and queries through one in-memory DuckDB database.

    Documents and queries share token_expression(), so a query is tokenized
    exactly the way the indexed documents were.

    tokenize_query() is thread-safe: a DuckDB connection is not, so each thread
    gets its own cursor (an independent connection to the same database) on first
    use. The build methods are single-threaded by design and run on con.

    Attributes:
        stop_word_list: The stopword regex applied before splitting.
        con: The DuckDB connection the build methods run on, and the parent of
            every thread's cursor.
    """

    def __init__(
        self
    ):
        """Opens an in-memory DuckDB connection."""
        self.stop_word_list = STOP_WORDS
        self.con = db.connect()
        self._local = threading.local()
        self._cursors = []
        self._cursors_lock = threading.Lock()

    def close(self) -> None:
        """Closes every thread's cursor and the connection, releasing their memory.

        Call this before the C++ build runs in the same process. The tokenizer
        cannot be used afterwards.
        """
        with self._cursors_lock:
            for cursor in self._cursors:
                cursor.close()
            self._cursors.clear()
        self.con.close()

    def _cursor(self) -> db.DuckDBPyConnection:
        """Returns the calling thread's DuckDB cursor, creating it on first use.

        Cursors are kept until close(), so a thread that exits leaves its cursor
        behind. Server threadpools reuse threads, so the count stays bounded.
        """
        cursor = getattr(self._local, "cursor", None)
        if cursor is None:
            with self._cursors_lock:
                cursor = self.con.cursor()
                self._cursors.append(cursor)
            self._local.cursor = cursor
        return cursor

    @staticmethod
    def token_stream_file_name(idx: int) -> str:
        """Returns the file name of one token stream file.

        Args:
            idx: Zero-based file index.

        Returns:
            A name like "token_0007.bin".
        """
        return f"token_{idx:04d}.bin"

    def build_doc_id_lookup(
        self,
        source_sql: str,
        lookup_file: Path,
        row_per_chunk: int
    ) -> np.ndarray:
        """Ranks every raw_id into a dense mapped_id in [0, N) and writes the lookup.

        raw_id is sparse and can exceed int32, so downstream structures
        (posting lists, doc lengths, graph adjacency) index by mapped_id
        instead, giving them O(1) flat-array access. mapped_id is the rank of
        raw_id, so only the ids are sorted. The file format is defined in
        startorch.lexical.doc_id_lookup.

        Args:
            source_sql: A source query yielding (raw_id, text) rows.
            lookup_file: Where to write doc_id_lookup.bin.
            row_per_chunk: Rows fetched from DuckDB per batch.

        Returns:
            Every raw_id in ascending order, so raw_ids[mapped_id] == raw_id.

        Raises:
            RuntimeError: If the raw ids are not unique.
        """
        logger.info(f"Building doc_id lookup table.")

        doc_count = fetch_one(self.con.sql(f"SELECT count(*) FROM ({source_sql})"))[0]
        sorted_ids = self.con.sql(f"SELECT raw_id FROM ({source_sql}) ORDER BY raw_id ASC")

        lookup_file.parent.mkdir(parents=True, exist_ok=True)
        logger.info(f"Writing doc_id lookup table to {lookup_file}.")

        raw_ids = np.empty(doc_count, dtype=np.int64)
        start = 0
        with open(lookup_file, "wb") as out_file:
            for batch in sorted_ids.to_arrow_reader(row_per_chunk):
                raw = batch.column("raw_id").to_numpy()
                end = start + len(raw)
                raw_ids[start:end] = raw
                out_file.write(lookup_records(raw, start).tobytes())
                start = end

        # Strictly increasing also rules out duplicate ids, which would make
        # two documents share a mapped_id.
        if start != doc_count or not np.all(raw_ids[1:] > raw_ids[:-1]):
            raise RuntimeError(f"doc_id lookup {lookup_file} is not {doc_count} unique, sorted ids.")

        logger.info(f"Finished writing doc_id lookup table ({lookup_file}).")
        return raw_ids

    @staticmethod
    def encode_token_records(tokens: pa.ListArray, mapped_ids: np.ndarray) -> tuple[pa.Buffer, int, int]:
        """Packs one batch of documents into token-stream records.

        Record layout (read_token in C++): little-endian int64 doc_id, uint16
        byte length, then the token's UTF-8 bytes. A document's tokens stay
        contiguous, which the C++ build relies on.

        Args:
            tokens: One list of token strings per document.
            mapped_ids: Each document's mapped_id, aligned with tokens.

        Returns:
            The record bytes, the token count, and the longest token in bytes.

        Raises:
            ValueError: If a token is longer than 65,535 bytes.
        """
        counts = pc.list_value_length(tokens).fill_null(0).to_numpy(zero_copy_only=False)
        flat = pc.list_flatten(tokens)
        token_count = len(flat)
        if token_count == 0:
            return pa.py_buffer(b""), 0, 0

        lengths = pc.binary_length(flat).to_numpy(zero_copy_only=False)
        longest = int(lengths.max())
        if longest > np.iinfo(np.uint16).max:
            raise ValueError(f"Token of {longest} bytes does not fit the uint16 length field.")

        header = np.empty(token_count, dtype=[("doc_id", "<i8"), ("length", "<u2")])
        header["doc_id"] = np.repeat(mapped_ids.astype(np.int64), counts)
        header["length"] = lengths
        header_bin = pa.FixedSizeBinaryArray.from_buffers(
            pa.binary(header.dtype.itemsize), token_count, [None, pa.py_buffer(header)]
        ).cast(pa.binary())

        # header_i + token_i for every token, as one contiguous value buffer.
        records = pc.binary_join_element_wise(header_bin, flat.cast(pa.binary()), b"")
        offsets = np.frombuffer(records.buffers()[1], dtype=np.int32)
        body = records.buffers()[2]
        return body[offsets[records.offset]:offsets[records.offset + token_count]], token_count, longest

    def get_token(
        self,
        source_sql: str,
        out_path: Path,
        lookup_file: Path,
        spill_path: Path,
        row_per_chunk: int = (1<<18),
        chunk_per_file: int = (1<<8),
        docs_per_batch: int = 50_000
    ):
        """Writes the doc_id lookup, then every document's tokens as a token stream.

        Documents are streamed in batches with no sort, join, or temp table,
        in whatever order DuckDB's scan threads produce them: the C++ build
        accepts any document order as long as each document's tokens are
        contiguous. Each batch's raw ids are mapped to mapped_ids with a
        binary search over the lookup's sorted raw ids. The lookup and the
        stream read the same source, so every id is found.

        A new token file starts once the current one holds
        row_per_chunk * chunk_per_file tokens. Token files left in out_path by
        an earlier run are removed first, so none of them reach the build.

        Args:
            source_sql: A source query yielding (raw_id, text) rows.
            out_path: Folder for the token_*.bin files.
            lookup_file: Where to write doc_id_lookup.bin.
            spill_path: DuckDB's spill directory for work that exceeds memory.
            row_per_chunk: Rows per DuckDB batch while building the lookup.
            chunk_per_file: With row_per_chunk, sets the tokens per output file.
            docs_per_batch: Documents tokenized and written per batch.

        Raises:
            RuntimeError: If the lookup's raw ids are not unique.
            KeyError: If a streamed document's raw id is not in the lookup.
            ValueError: If a token is longer than 65,535 bytes.
        """
        self.con.execute(f"SET temp_directory = '{spill_path}'")
        self.con.sql("INSTALL fts; LOAD fts;")

        logger.info(f"Start serializing corpus.")

        raw_ids = self.build_doc_id_lookup(source_sql, lookup_file, row_per_chunk)

        # Unordered output lets the parallel scan hand batches over as soon as
        # they are ready instead of waiting on the slowest thread (3.6x faster
        # on math-en). Queries with ORDER BY keep their order regardless.
        self.con.execute("SET preserve_insertion_order = false")

        tokenized = self.con.sql(f"""
            SELECT raw_id, {token_expression("text")} AS tokens
            FROM ({source_sql})
        """)

        tokens_per_file = row_per_chunk * chunk_per_file
        file_count = 0
        file_tokens = 0
        out_file = None

        no_tokens = 0
        max_token_length = 0

        out_path.mkdir(parents=True, exist_ok=True)
        stale = sorted(out_path.glob("token_*.bin"))
        if stale:
            logger.info(f"Removing {len(stale)} token files left in {out_path} by an earlier run.")
            for path in stale:
                path.unlink()
        logger.info(f"Begin serializing token to {str(out_path)}.")

        try:
            for batch in tokenized.to_arrow_reader(docs_per_batch):
                mapped_ids = map_raw_ids(raw_ids, batch.column("raw_id").to_numpy())
                records, token_count, longest = self.encode_token_records(batch.column("tokens"), mapped_ids)

                if out_file is None or file_tokens >= tokens_per_file:
                    if out_file is not None:
                        out_file.close()
                        file_count += 1
                    file_name = self.token_stream_file_name(file_count)
                    logger.info(f"Outputting stream into file {file_name}.")
                    out_file = open(out_path / file_name, "wb")
                    file_tokens = 0

                out_file.write(records)
                file_tokens += token_count
                no_tokens += token_count
                max_token_length = max(max_token_length, longest)
        finally:
            if out_file is not None:
                out_file.close()

        logger.info(f"Finished serializing corpus.")
        logger.info(f"No of tokens: {no_tokens}")
        logger.info(f"Max token length = {max_token_length}")

    def tokenize_query(self, query: str) -> list[str]:
        """Tokenizes a query exactly the way documents were tokenized.

        Thread-safe: runs on the calling thread's own cursor.

        Args:
            query: Free-text query.

        Returns:
            The query's stemmed tokens, stopwords removed. May be empty.
        """
        res = fetch_one(self._cursor().sql(
            f"SELECT {token_expression('$query')} AS tokens",
            params={"query": query}
        ))

        return res[0]
