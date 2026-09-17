"""Makes a smaller Works subset from the full corpus, for development and testing.

Driven by the CLI (startorch gen-works-subset) and project-config.toml.
"""

import duckdb as db
import math
import tomllib

from pathlib import Path
from startorch import get_logger, get_current_time, PROJECT_ROOT

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

    def validate_database(self):
        """Checks subset_path against full_corpus_path: filter match, duplicate IDs, link counts.

        Writes database_validation_log.txt under subset_path. Every check is a SQL
        aggregate/count query - no per-row Python loop, no full id set held in memory.
        The straightforward per-row version OOMs at full-corpus scale (hundreds of
        millions of ids held twice over, once as full-corpus matches and once as
        subset-present, plus a growing error-message list) - this trades per-row detail
        for counts only, computed inside DuckDB's own (spill-configured) engine instead.
        """
        con = db.connect(config={"temp_directory": str(self.spill_path)})

        logger.info("Begin validating Database")

        logger.info("Checking total nodes/links, filter mismatches, link-count mismatches...")
        total_nodes, total_links, filter_mismatch_count, link_mismatch_count = con.sql(f"""
            SELECT
                count(DISTINCT id) AS total_nodes,
                sum(referenced_works_count) AS total_links,
                count(*) FILTER (WHERE NOT matches_filter) AS filter_mismatch_count,
                count(*) FILTER (
                    WHERE referenced_works_count IS DISTINCT FROM len(referenced_works)
                ) AS link_mismatch_count
            FROM (
                SELECT
                    id, referenced_works, referenced_works_count,
                    ({self.filter_condition}) AS matches_filter
                FROM read_parquet('{str(self.subset_path)}/**/*.parquet')
            )
        """).fetchone()

        logger.info("Checking duplicate ids...")
        duplicate_id_count = con.sql(f"""
            SELECT count(*) FROM (
                SELECT id
                FROM read_parquet('{str(self.subset_path)}/**/*.parquet')
                GROUP BY id
                HAVING count(*) > 1
            )
        """).fetchone()[0]

        logger.info("Checking for full-corpus entries missing from subset...")
        missing_entry_count = con.sql(f"""
            SELECT count(*)
            FROM (
                SELECT id
                FROM read_parquet('{self.full_corpus_path}/works/**/*.parquet')
                WHERE {self.filter_condition}
            ) f
            ANTI JOIN read_parquet('{str(self.subset_path)}/**/*.parquet') s ON f.id = s.id
        """).fetchone()[0]

        logger.info("Checking dangling references (pointing outside the subset)...")
        dangling_links = con.sql(f"""
            SELECT count(*)
            FROM (
                SELECT unnest(referenced_works) AS ref
                FROM read_parquet('{str(self.subset_path)}/**/*.parquet')
            ) w
            ANTI JOIN read_parquet('{str(self.subset_path)}/**/*.parquet') s ON s.id = w.ref
        """).fetchone()[0]

        total_errors = filter_mismatch_count + duplicate_id_count + link_mismatch_count + missing_entry_count

        logger.info("Finished validating Database")
        logger.info(f"Total nodes: {total_nodes}.")
        logger.info(f"Total links: {total_links}.")
        logger.info(f"Total dangling links: {dangling_links}.")
        logger.info(f"Total valid links: {total_links - dangling_links}.")
        logger.info(f"Filter mismatches: {filter_mismatch_count}.")
        logger.info(f"Duplicate ids: {duplicate_id_count}.")
        logger.info(f"Link count mismatches: {link_mismatch_count}.")
        logger.info(f"Missing entries (in full corpus, not in subset): {missing_entry_count}.")
        logger.info(f"Total errors: {total_errors}.")

        try:
            with open(self.subset_path/"database_validation_log.txt", "w", encoding="utf-8") as f:
                f.write(f"Time created: {get_current_time()}.\n")
                f.write(f"Total nodes: {total_nodes}.\n")
                f.write(f"Total links: {total_links}.\n")
                f.write(f"Total dangling links: {dangling_links}.\n")
                f.write(f"Total valid links: {total_links - dangling_links}.\n")
                f.write(f"Filter mismatches: {filter_mismatch_count}.\n")
                f.write(f"Duplicate ids: {duplicate_id_count}.\n")
                f.write(f"Link count mismatches: {link_mismatch_count}.\n")
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
        logger.info("Established connection to DB")

        logger.info("Started fetching subset.")

        groups_per_file = 4
        row_group_size = max(2048, self.rows_per_chunk // groups_per_file)

        n_rows = con.execute(f"""
            COPY (
                SELECT *
                FROM read_parquet('{self.full_corpus_path}/works/**/*.parquet')
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


def _resolve(raw: str) -> Path:
    p = Path(raw)
    return p if p.is_absolute() else (PROJECT_ROOT / p).resolve()


def main():
    """Dev entry point: runs validate_database() only, against the full-en profile.

    Assumes the full-en subset already exists at its configured path.
    """
    with open(PROJECT_ROOT / "project-config.toml", "rb") as f:
        config = tomllib.load(f)

    paths = config["data-path"]
    data_path = _resolve(paths["data-path"])
    full_corpus_path = data_path / paths["full-corpus-folder"]
    subset_path = data_path / paths["works-subset-folder"] / "full-en"
    spill_path = _resolve(config["duckdb"]["spill-path"])
    condition = config["works-subset"]["subset-profiles"]["full-en"]

    subsetter = WorksSubsetter(full_corpus_path, subset_path, spill_path, condition)
    subsetter.validate_database()

if __name__ == "__main__":
    main()
