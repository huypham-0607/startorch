"""Makes a smaller Works subset from the full corpus, for development and testing.

Driven by the CLI (startorch gen-works-subset) and project-config.toml. Only
profiles with materialize = true are copied; the rest are read straight from
the corpus through their filter (see startorch.tokenizer.profile_source).
"""

import sys

import duckdb as db

from pathlib import Path
from startorch.utils import fetch_one, get_current_time, get_logger

logger = get_logger(__name__)


class WorksSubsetter:
    """Filters full_corpus_path/works by filter_condition, writes the result to subset_path.

    full_corpus_path is the corpus root (the folder that holds works/), not works/ itself.
    """

    def __init__(
        self,
        full_corpus_path: Path,
        subset_path: Path,
        spill_path: Path,
        filter_condition: str,
        rows_per_chunk: int = 2000000
    ):
        logger.info("Initializing WorksSubsetter.")

        self.full_corpus_path = full_corpus_path
        self.subset_path = subset_path
        self.spill_path = spill_path
        self.filter_condition = filter_condition
        self.rows_per_chunk = rows_per_chunk

        logger.info("Finished initializing WorksSubsetter.")

    def _corpus_glob(self) -> str:
        return f"{self.full_corpus_path}/works/**/*.parquet"

    def count_matching(self) -> int:
        """Works in the full corpus matching the filter. Reads only the filter's columns."""
        con = db.connect(config={"temp_directory": str(self.spill_path)})
        return fetch_one(con.sql(f"""
            SELECT count(*) FROM read_parquet('{self._corpus_glob()}') WHERE {self.filter_condition}
        """))[0]

    def validate_database(self):
        """Checks subset_path against the corpus: every row matches the filter,
        ids are unique, and no matching work is missing.

        Writes database_validation_log.txt under subset_path. Two queries:

        - one scan of the subset: row count, distinct ids (parsed to BIGINT, so
          the hash table holds 8-byte keys rather than strings), filter mismatches;
        - the corpus's filtered row count, which reads only the filter's columns.

        Missing entries = corpus count - subset rows. That is exact: the subset
        was copied from the corpus, its ids are distinct, and every row matches
        the filter, so equal counts mean equal sets.
        """
        con = db.connect(config={"temp_directory": str(self.spill_path)})

        logger.info("Begin validating Database")

        logger.info("Checking total nodes, duplicate ids, filter mismatches...")
        total_rows, total_nodes, total_links, filter_mismatch_count = fetch_one(con.sql(f"""
            SELECT
                count(*),
                count(DISTINCT regexp_replace(id, 'W', '')::BIGINT),
                sum(referenced_works_count),
                count(*) FILTER (WHERE NOT ({self.filter_condition}))
            FROM read_parquet('{str(self.subset_path)}/**/*.parquet')
        """))
        duplicate_id_count = total_rows - total_nodes

        logger.info("Checking for full-corpus entries missing from subset...")
        missing_entry_count = self.count_matching() - (total_rows - filter_mismatch_count)

        total_errors = filter_mismatch_count + duplicate_id_count + missing_entry_count

        logger.info("Finished validating Database")
        logger.info(f"Total nodes: {total_nodes}.")
        logger.info(f"Total links: {total_links}.")
        logger.info(f"Filter mismatches: {filter_mismatch_count}.")
        logger.info(f"Duplicate ids: {duplicate_id_count}.")
        logger.info(f"Missing entries (in full corpus, not in subset): {missing_entry_count}.")
        logger.info(f"Total errors: {total_errors}.")

        try:
            with open(self.subset_path/"database_validation_log.txt", "w", encoding="utf-8") as f:
                f.write(f"Time created: {get_current_time()}.\n")
                f.write(f"Total nodes: {total_nodes}.\n")
                f.write(f"Total links: {total_links}.\n")
                f.write(f"Filter mismatches: {filter_mismatch_count}.\n")
                f.write(f"Duplicate ids: {duplicate_id_count}.\n")
                f.write(f"Missing entries (in full corpus, not in subset): {missing_entry_count}.\n")
                f.write(f"Total errors: {total_errors}.\n")
        except Exception as e:
            logger.warning(f"Failed to open database_validation_log.txt: {e}")
            raise

    def subset_database(self):
        """Filters full_corpus_path/works by filter_condition, writes subset_path in chunks."""
        logger.info("Connecting to DB.")
        self.subset_path.mkdir(parents=True, exist_ok=True)

        con = db.connect(config={"temp_directory": str(self.spill_path)})
        # Row order doesn't matter to anything downstream (the tokenizer and
        # the C++ build accept any order), so let the scan threads write
        # without buffering batches to keep file order.
        con.execute("SET preserve_insertion_order = false")
        logger.info("Established connection to DB")

        logger.info("Started fetching subset.")

        groups_per_file = 4
        row_group_size = max(2048, self.rows_per_chunk // groups_per_file)

        n_rows = con.execute(f"""
            COPY (
                SELECT *
                FROM read_parquet('{self._corpus_glob()}')
                WHERE {self.filter_condition}
            )
            TO '{str(self.subset_path)}'
            (FORMAT PARQUET, COMPRESSION zstd,
            ROW_GROUP_SIZE {row_group_size},
            ROW_GROUPS_PER_FILE {groups_per_file},
            OVERWRITE_OR_IGNORE)
        """).fetchall()[0][0]

        n_files = len(list(Path(self.subset_path).glob("*.parquet")))
        logger.info(f"Filtered subset: {n_rows} rows -> {n_files} files "
                    f"of ~{self.rows_per_chunk} rows each")

        logger.info("Subset saved")


def main():
    """Dev entry point: validate one copied profile's existing subset.

    python -m startorch.works_subset.works_subset [profile]   (default math-en)
    """
    from startorch.paths import corpus_paths, profile

    p = profile(sys.argv[1] if len(sys.argv) > 1 else "math-en")
    if not p.materialize:
        raise SystemExit(f"Profile {p.name} is not copied into a subset; nothing to validate.")
    assert p.subset_dir is not None and p.filter is not None, f"{p.name} is not an OpenAlex profile"
    subsetter = WorksSubsetter(corpus_paths().compact, p.subset_dir, p.spill_dir, p.filter)
    subsetter.validate_database()

if __name__ == "__main__":
    main()
