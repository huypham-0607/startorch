"""Gets shards from the OpenAlex snapshot and writes a compact copy.

Start at EntityIngestor.orchestrate(). WorksIngestor is the current entity class.
"""

import boto3
import duckdb
import json
import os

from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from startorch.utils.duckdb import fetch_one
from startorch.utils.logger import get_logger
from startorch.utils.misc import get_current_time
from botocore import UNSIGNED
from botocore.client import Config

logger = get_logger(__name__)


class EntityIngestor(ABC):
    """Base class for OpenAlex ingestion. Each entity needs one subclass.

    A subclass sets extracted_columns/columns/entity, and writes the abstract methods.
    """

    extracted_columns: list[str]
    columns: list[str]
    entity: str
    registry: dict[str, type["EntityIngestor"]] = {}

    @dataclass(kw_only=True)
    class ManifestData:
        """One entry from a manifest.json file: an S3 key and the raw shard's stats."""
        key: Path               # Key (path) of the shard on S3, also the local path fragment
        content_length: int     # Raw file size, in bytes
        record_count: int       # Number of rows in the shard

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        EntityIngestor.registry[cls.entity] = cls

    def __init__(self, upstream_prefix: Path, raw_path: Path, compact_path: Path):
        self.upstream_prefix = upstream_prefix
        self.raw_path = raw_path
        self.compact_path = compact_path

    @property
    @abstractmethod
    def entity(self) -> str:
        raise NotImplementedError

    def get_manifest_data(self, manifest_path: Path) -> list:
        """Reads a manifest.json file into a list of ManifestData.

        Args:
            manifest_path: Path to a manifest.json file on disk.

        Returns:
            A list of ManifestData, one per shard in the manifest.

        Raises:
            FileNotFoundError or json.JSONDecodeError if the file is missing or bad.
            KeyError if the file's format does not match the expected OpenAlex shape.
        """

        manifest_data = None
        try:
            with open(manifest_path, 'r') as f:
                manifest_data = json.load(f)
        except Exception as e:
            logger.warning(f"Unable to open manifest.json: {e}")
            raise

        # Getting all files metadata in manifest.json, and stripping s3 prefix.
        lst = [self.ManifestData(
            key = Path(obj["url"].replace('s3://openalex/data/parquet/', '')) if ("url" in obj)
                    else obj["key"],
            content_length = obj['meta']['content_length'],
            record_count = obj['meta']['record_count']
        ) for obj in manifest_data["files"]]

        return lst

    def get_manifest(self, upstream_path, dest_path: Path) -> None:
        """Downloads this entity's manifest.json file from OpenAlex on S3.

        Args:
            upstream_path: Key of manifest.json in the 'openalex' S3 bucket.
            dest_path: Local path to write the manifest.json file to.

        Returns:
            None

        Raises:
            A network or S3 error if the download fails, or OSError if the write fails.
        """

        s3 = boto3.client('s3', config=Config(signature_version=UNSIGNED))
        try:
            s3.download_file('openalex', str(upstream_path), str(dest_path))
        except Exception as e:
            logger.warning(f"Failed to fetch {upstream_path}.")
            raise

        logger.info(f"Fetched {upstream_path} successfully.")

    def list_local_and_remote_shards(self, manifest_path: Path, raw_path: Path) -> (list, list):
        """Splits manifest entries into shards already on disk and shards that are not.

        Args:
            manifest_path: Path to a manifest.json file on disk.
            raw_path: Root folder that holds one subfolder per entity.

        Returns:
            Two lists of ManifestData: local shards, then remote shards.

        Raises:
            Same errors as get_manifest_data.
        """

        data = self.get_manifest_data(manifest_path)

        remote_shard_list = []
        local_shard_list = []

        for obj in data:
            # Extracting only the directory of a shard (Hardcoded, subject to edits)
            if (not Path(raw_path/obj.key).is_file()):
                remote_shard_list.append(obj)
            else:
                local_shard_list.append(obj)

        return (local_shard_list, remote_shard_list)

    def fetch_shard(self, upstream_path, dest_path: Path) -> None:
        """Downloads one raw shard from OpenAlex on S3.

        Args:
            upstream_path: Key of the shard on S3 (ManifestData.key).
            dest_path: Local path to save the shard to.

        Returns:
            None

        Raises:
            A network or S3 error if the download fails, or OSError if the write fails.
        """

        dest_path.parent.mkdir(parents=True, exist_ok=True)
        s3 = boto3.client('s3', config=Config(signature_version=UNSIGNED))
        try:
            s3.download_file('openalex', str(upstream_path), str(dest_path))
        except Exception as e:
            logger.warning(f"Failed to fetch {upstream_path}.")
            raise

    def show_shard(self, shard_path: Path):
        """Prints a parquet file's contents to the screen. For manual checks only.

        Args:
            shard_path: Path to a parquet file, raw or compact.

        Returns:
            None

        Raises:
            A DuckDB error if the file is missing or not valid parquet.
        """
        db = duckdb.connect()
        rel = db.read_parquet(shard_path)

        rel.show()

    def delete_shard(self, shard_path: Path) -> None:
        """Deletes a shard file. This is permanent.

        Args:
            shard_path: Path to the shard to delete.

        Returns:
            None

        Raises:
            OSError if the file exists but cannot be deleted. A missing file is not an error.
        """

        shard_path.unlink(missing_ok=True)

    def init_extraction_log(self, extraction_log_path: Path) -> None:
        """Creates a new extraction log file. This overwrites any old log.

        Args:
            extraction_log_path: Path to the log file to create.

        Returns:
            None

        Raises:
            OSError if the file cannot be created.
        """
        try:
            with open(extraction_log_path, "w", encoding="utf-8") as f:
                f.write(f"Time created: {get_current_time()}\n")
        except Exception as e:
            logger.warning(f"Failed to open extraction_log.txt: {e}")
            raise

    def append_extraction_log(self, extraction_log_path: Path, message: str) -> None:
        """Adds one line to the extraction log file.

        Args:
            extraction_log_path: Path to an existing log file.
            message: Text to add. A newline is added after it.

        Returns:
            None

        Raises:
            OSError if the file does not exist or cannot be written.
        """
        try:
            with open(extraction_log_path, "a", encoding="utf-8") as f:
                f.write(f"{message}\n")
        except Exception as e:
            logger.warning(f"Failed to open extraction_log.txt: {e}")
            raise

    @abstractmethod
    def extract_compact(self, shard_path, out_path: Path):
        """Converts one raw shard to the compact format. Reads shard_path, writes out_path.

        Args:
            shard_path: Path to the raw shard to read.
            out_path: Path to write the compact shard to.

        Returns:
            None
        """
        raise NotImplementedError

    @abstractmethod
    def validate_compact_data(self, manifest_path, compact_path: Path):
        """Checks every compact shard for this entity, together.

        Args:
            manifest_path: Path to the entity's manifest.json file.
            compact_path: Root folder of the compact output.

        Returns:
            None
        """
        raise NotImplementedError

    @abstractmethod
    def validate_compact_shard(self, file: ManifestData) -> list[str]:
        """Checks one compact shard and lists any problems found.

        Args:
            file: The ManifestData entry for the shard to check.

        Returns:
            A list of error messages. An empty list means the shard passed.
        """
        raise NotImplementedError

    def orchestrate(self, forced_fetch: bool = False):
        """Fetches, converts, and checks every shard for this entity.

        Deletes each raw shard once it passes its checks. Writes extraction_log.txt,
        and integrity_report.txt/compact_manifest.json under compact_path.

        Args:
            forced_fetch: If True, fetch a shard again even if a compact copy exists.

        Returns:
            None

        Raises:
            RuntimeError if 50 or more shards fail their checks.
        """
        upstream_manifest_path = self.upstream_prefix/self.entity/"manifest.json"
        manifest_path = self.raw_path/self.entity/"manifest.json"

        Path(self.raw_path/self.entity).mkdir(parents=True, exist_ok=True)
        Path(self.compact_path/self.entity).mkdir(parents=True, exist_ok=True)

        self.get_manifest(upstream_manifest_path, manifest_path)

        local_shards, remote_shards = self.list_local_and_remote_shards(manifest_path, self.raw_path)

        extraction_log_path = self.compact_path/self.entity/"extraction_log.txt"

        # Setting a limit for how many invalid shards before raising exception.
        invalid_shard_limit = 50
        invalid_shard_count = 0

        self.init_extraction_log(extraction_log_path)

        logger.info(f"Initiating shard extraction for entity {self.entity}")

        for file in local_shards:
            logger.info(f"Current shard: {file.key} [local].")
            logger.info(f"Extracting shard {file.key}...")

            self.extract_compact(self.raw_path/file.key, self.compact_path/file.key)
            logger.info(f"Extracted shard {file.key} successsfully, validating shard...")

            errors = self.validate_compact_shard(file)

            if len(errors) != 0:
                invalid_shard_count += 1
                for error in errors:
                    logger.warning(error)
                    self.append_extraction_log(extraction_log_path, error)
            else:
                self.delete_shard(self.raw_path/file.key)
                logger.info(f"Validated shard {file.key} successfully: No errors found.")

            if invalid_shard_count >= invalid_shard_limit:
                logger.warning(f"Invalid shard count exceeded limit {invalid_shard_limit}, raising...")
                raise RuntimeError(f"Invalid shard limit exceeded (limit: {invalid_shard_limit}).")

        for file in remote_shards:
            # Raw files not on local & compact file exist implies shard passed validation check.
            if (not forced_fetch and Path(self.compact_path/file.key).is_file()):
                continue

            logger.info(f"Current shard: {file.key} [remote].")
            logger.info(f"Fetching shard {file.key}...")

            self.fetch_shard(self.upstream_prefix/file.key, self.raw_path/file.key)
            logger.info(f"Fetched shard {file.key} successfully, extracting shard...")

            self.extract_compact(self.raw_path/file.key, self.compact_path/file.key)
            logger.info(f"Extracted shard {file.key} successsfully, validating shard...")

            errors = self.validate_compact_shard(file)

            if len(errors) != 0:
                invalid_shard_count += 1
                for error in errors:
                    logger.warning(error)
                    self.append_extraction_log(extraction_log_path, error)
            else:
                self.delete_shard(self.raw_path/file.key)
                logger.info(f"Validated shard {file.key} successfully: No errors found.")

            if invalid_shard_count >= invalid_shard_limit:
                logger.warning(f"Invalid shard count exceeded limit {invalid_shard_limit}, raising...")
                raise RuntimeError(f"Invalid shard limit exceeded (limit: {invalid_shard_limit}).")

        if invalid_shard_count != 0:
            logger.info(f"""
                Orchestration completed - localized shard errors detected
                (invalid shards: {invalid_shard_count})").\n
                Please navigate to {extraction_log_path} for more details.
            """)
            return

        logger.info(f"Finished shard extraction, validating all compact data...")
        self.validate_compact_data(manifest_path, self.compact_path)
        logger.info(f"""
            Orchestration completed - no localized shard errors.\n
            Please navigate to {self.compact_path/self.entity/"integrity_report.txt"} for comprehensive analysis.
        """)


class WorksIngestor(EntityIngestor):
    """The Works entity. The only entity class right now."""

    # Columns being extracted from raw shards
    extracted_columns = [
        "id",
        "doi",
        "title",
        "authorships",
        "type",
        "language",
        "primary_location",
        "publication_year",
        "publication_date",
        "referenced_works",
        "referenced_works_count",
        "cited_by_count",
        "topics",
        "keywords"
    ]

    # Columns present in final compact shard
    columns = [
        "id",
        "doi",
        "title",
        "authorships",
        "authorships_truncated",
        "type",
        "language",
        "primary_location",
        "publication_year",
        "publication_date",
        "referenced_works",
        "referenced_works_count",
        "cited_by_count",
        "topics",
        "keywords"
    ]

    entity = "works"

    @dataclass(kw_only=True)
    class _ShardValidationResult:
        """Holds the raw and compact stats for one shard. Used only inside this class."""
        raw_content_length: int
        compact_content_length: int
        raw_record_count: int
        compact_record_count: int
        link_mismatch_count: int
        is_valid_schema: bool
        missing_columns: list[str]
        redundant_columns: list[str]

    def extract_compact(self, shard_path, out_path: Path):
        """Converts one raw Works shard to compact form, keeping at most 3 authors per work.

        Writes out_path as a parquet file, compressed with zstd.

        Args:
            shard_path: Path to the raw shard to read.
            out_path: Path to write the compact shard to.

        Returns:
            None

        Raises:
            A DuckDB error if the raw shard's format is not as expected.
        """

        db = duckdb.connect()
        rel = db.read_parquet(shard_path).select(*self.extracted_columns)

        # Number of records is an invariant after the transform. Count here for cheaper computation.
        record_count = fetch_one(rel.aggregate("count(*)"))[0]
        rel = rel.select("""
            * REPLACE (
                replace(id, 'https://openalex.org/', '') AS id,

                list_transform(
                    CASE WHEN len(authorships) <= 3
                        THEN authorships
                        ELSE authorships[1:2] || [authorships[-1]]
                    END,
                    a -> {
                        'id': replace(a.author.id, 'https://openalex.org/', ''),
                        'display_name': a.author.display_name,
                        'raw_author_name': a.raw_author_name
                    }
                ) AS authorships,

                CASE WHEN primary_location.source IS NULL THEN NULL
                    ELSE {
                        'id': replace(primary_location.source.id, 'https://openalex.org/', ''),
                        'display_name': primary_location.source.display_name,
                        'type': primary_location.source.type,
                        'is_oa': primary_location.is_oa
                    }
                END AS primary_location,

                list_transform(topics, a -> {
                    'id': replace(a.id, 'https://openalex.org/', ''),
                    'display_name': a.display_name,
                    'score': a.score,
                    'subfield_id': split_part(a.subfield.id, '/', -1),
                    'subfield_display_name': a.subfield.display_name,
                    'field_id': split_part(a.field.id, '/', -1),
                    'field_display_name': a.field.display_name,
                    'domain_id': split_part(a.domain.id, '/', -1),
                    'domain_display_name': a.domain.display_name
                }) AS topics,

                replace(doi, 'https://doi.org/', '') AS doi,

                list_transform(keywords, a -> {
                    'id': replace(a.id, 'https://openalex.org/', ''),
                    'display_name': a.display_name,
                    'score': a.score
                }) AS keywords,

                list_transform(referenced_works, a -> replace(a, 'https://openalex.org/', ''))
                    AS referenced_works,
            ),
            coalesce(len(authorships), 0) > 3 AS authorships_truncated,
        """)

        out_path.parent.mkdir(parents = True, exist_ok = True)

        db.sql(f"""
            COPY (SELECT * FROM rel)
            TO '{out_path}'
            (FORMAT parquet, COMPRESSION zstd)
        """)

    def _get_shard_validation_result(self, shard_path: Path, content_length, record_count: int):
        """Measures one compact Works shard and compares it to the manifest.

        Uses the referenced_works field, so this only works for the Works schema.

        Args:
            shard_path: Path to the compact shard to measure.
            content_length: Raw file size, from the manifest.
            record_count: Raw row count, from the manifest.

        Returns:
            A _ShardValidationResult with the measured values.

        Raises:
            A DuckDB error, or OSError if shard_path does not exist.
        """

        db = duckdb.connect()

        rel = db.read_parquet(shard_path)
        cols = rel.columns
        compact_record_count = fetch_one(rel.aggregate("count(*) AS total"))[0]
        compact_content_length = os.path.getsize(shard_path)

        # referenced_works_count vs actual list length - NULL counts as a mismatch
        link_mismatch_count = fetch_one(rel.aggregate(
            "count(*) FILTER (WHERE referenced_works_count IS DISTINCT FROM len(referenced_works)) AS bad"
        ))[0]

        # column-set check against the expected schema
        expected_columns = self.columns
        missing_columns   = [c for c in expected_columns if c not in cols]
        redundant_columns = [c for c in cols if c not in expected_columns]

        res = self._ShardValidationResult(
            raw_content_length = content_length,
            compact_content_length = compact_content_length,
            raw_record_count = record_count,
            compact_record_count = compact_record_count,
            link_mismatch_count = link_mismatch_count,
            is_valid_schema = (len(missing_columns) == 0 and len(redundant_columns) == 0),
            missing_columns = missing_columns,
            redundant_columns = redundant_columns,
        )

        return res

    def validate_compact_shard(self, file: EntityIngestor.ManifestData) -> list[str]:
        """Checks one compact shard: row count, link count, and schema.

        Args:
            file: The ManifestData entry for the shard to check.

        Returns:
            A list of error messages. An empty list means the shard passed.

        Raises:
            Same errors as _get_shard_validation_result.
        """
        val_result = self._get_shard_validation_result(
            self.compact_path/file.key,
            file.content_length,
            file.record_count
        )

        errors = []

        if val_result.raw_record_count != val_result.compact_record_count:
            message = f"""
                Shard {file.key} - Mismatched record_count detected.
                raw_record_count is {val_result.raw_record_count},
                compact_record_count is {val_result.compact_record_count}.
            """
            errors.append(message)

        if val_result.link_mismatch_count != 0:
            message = f"""
                Shard {file.key} - Mismatched referenced_work length detected
                (Count: {val_result.link_mismatch_count}).
            """
            errors.append(message)

        if not val_result.is_valid_schema:
            message = f"""
                Shard {file.key} - Mismatched schema detected.
                missing_columns: {val_result.missing_columns};
                redundant_columns: {val_result.redundant_columns}.
            """
            errors.append(message)

        return errors

    def validate_compact_data(self, manifest_path, compact_path: Path):
        """Checks every compact Works shard together: duplicate IDs and reference links.

        Writes integrity_report.txt and compact_manifest.json under compact_path.

        Args:
            manifest_path: Path to the entity's manifest.json file.
            compact_path: Root folder of the compact output.

        Returns:
            None. Also prints a summary to the screen.

        Raises:
            A DuckDB error, or OSError if compact_path does not exist.
        """

        files = self.get_manifest_data(manifest_path)

        compact_shard_paths = [compact_path/file.key for file in files]

        total_raw_bytes = sum([file.content_length for file in files])

        con = duckdb.connect()

        # __ PASS 1: collect all ids (needed before any reference check) _____________

        compact_manifest = {
            "format": "parquet",
            "entity": self.entity,
            "record_count": 0,
            "content_length": 0,
            "files": []
        }
        total_bytes = 0
        for path in compact_shard_paths:
            p = str(path)

            # Getting record count & size per shard
            record_count = fetch_one(con.execute(
                "SELECT count(*) FROM read_parquet(?)", [p]
            ))[0]
            size = os.path.getsize(p)

            compact_manifest["files"].append({
                "key": str(Path(p).relative_to(compact_path)),
                "meta": {
                    "content_length": size,
                    "record_count": record_count
                }
            })
            compact_manifest["record_count"] += record_count
            compact_manifest["content_length"] += size
            total_bytes += size

        shard_paths = [str(p) for p in compact_shard_paths]

        # __ CHECK 1: Work ID uniqueness — list of (id, occurrences) __________________
        logger.info("Validating Work ID uniqueness...")
        duplicate_ids = con.sql("""
            SELECT id, count(*) AS n
            FROM read_parquet(?)
            GROUP BY id
            HAVING count(*) > 1
        """, params=[shard_paths]).fetchall()

        # __ CHECK 2a: dangling references — list of (work_id, referenced_work_id) ___
        logger.info("Computing dangling references...")
        dangling_refs = con.sql(f"""
            SELECT w.id AS work_id, w.ref AS referenced_work_id
            FROM (
                SELECT id, unnest(referenced_works) AS ref
                FROM read_parquet(?)
            ) w
            ANTI JOIN read_parquet(?) a ON a.id = w.ref
        """, params=[shard_paths, shard_paths]).fetchall()

        # __ CHECK 2b: duplicate links within a record ________________________________
        # Two-phase: first find candidate works with an internal duplicate via a cheap
        # length comparison (no unnest, full corpus scan but no row explosion), then
        # unnest only those candidates for per-reference detail. Unnesting all ~2.37B
        # referenced_works entries corpus-wide up front OOMs (verified); this doesn't,
        # since only works that already have a known duplicate ever get unnested.
        logger.info("Checking for duplicate links...")
        dup_link_detail = con.sql("""
            WITH candidates AS (
                SELECT id, referenced_works
                FROM read_parquet(?)
                WHERE len(referenced_works) <> len(list_distinct(referenced_works))
            )
            SELECT id AS work_id, ref AS referenced_work_id, count(*) AS occurrences
            FROM (
                SELECT id, unnest(referenced_works) AS ref
                FROM candidates
            )
            GROUP BY id, ref
            HAVING count(*) > 1
            ORDER BY work_id, occurrences DESC
        """, params=[shard_paths]).fetchall()

        # Derived from dup_link_detail, not a second corpus scan.
        dup_link_works = sorted({work_id for work_id, _, _ in dup_link_detail})

        # __ REPORT _________________________________________________________________
        print(f"Shards:               {len(compact_shard_paths)}")
        print(f"Total raw size:       {total_raw_bytes:,} bytes ({total_raw_bytes/1e9:.2f} GB)")
        print(f"Total size:           {total_bytes:,} bytes ({total_bytes/1e9:.2f} GB)")
        print(f"Size reduction:       {float(total_bytes)/total_raw_bytes*100:.2f}% of raw size")
        print(f"Duplicate work ids:   {len(duplicate_ids)}")
        print(f"Dangling references:  {len(dangling_refs)}")
        print(f"Works w/ dup links:   {len(dup_link_works)}")

        with open(compact_path/self.entity/"integrity_report.txt", "w", encoding="utf-8") as f:
            f.write(f"Shards: {len(compact_shard_paths)}\n")
            f.write(f"Total raw size: {total_raw_bytes:,} bytes ({total_raw_bytes/1e9:.2f} GB)\n")
            f.write(f"Total size: {total_bytes:,} bytes ({total_bytes/1e9:.2f} GB)\n")
            f.write(f"Size reduction: {float(total_bytes)/total_raw_bytes*100:.2f}% of raw size\n\n")

            f.write(f"Duplicate work ids ({len(duplicate_ids)}):\n")
            f.write(f"Dangling references ({len(dangling_refs)}):\n")
            f.write(f"Works with duplicate links ({len(dup_link_works)}):\n")
            f.write(f"Duplicate work ids list:\n")
            for wid, n in duplicate_ids:
                f.write(f"  {wid}\t{n}\n")
            f.write(f"Dangling refs list:\n")
            for work_id, ref_id in dangling_refs:
                f.write(f"  {work_id} -> {ref_id}\n")
            f.write(f"Works with duplicate links list:\n")
            for wid in dup_link_works:
                f.write(f"  {wid}\n")
            f.write(f"\nDuplicate link detail ({len(dup_link_detail)}):\n")
            for work_id, ref_id, n in dup_link_detail:
                f.write(f"  {work_id} -> {ref_id} (x{n})\n")






        with open(compact_path/self.entity/"compact_manifest.json", "w") as f:
            json.dump(compact_manifest, f)