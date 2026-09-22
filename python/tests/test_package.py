"""Package wiring: imports, class registration, config consistency and golden paths.

Ported from the old script/smoke_test.py. Nothing here touches S3, DuckDB files
or corpus data.
"""

import importlib
from pathlib import Path

import duckdb
import pytest

import startorch
from startorch import startorch_cpp
from startorch.ingest.fetch_data import EntityIngestor, WorksIngestor
from startorch.ingest.works_subset import WorksSubsetter
from startorch.utils.duckdb import fetch_one
from startorch.utils.paths import profile, profile_names

MODULES = [
    "startorch",
    "startorch.cli",
    "startorch.api.api",
    "startorch.api.schemas",
    "startorch.ingest.fetch_data",
    "startorch.ingest.works_subset",
    "startorch.lexical.build_posting",
    "startorch.lexical.doc_id_lookup",
    "startorch.lexical.search",
    "startorch.lexical.tokenizer",
    "startorch.utils.duckdb",
    "startorch.utils.logger",
    "startorch.utils.misc",
    "startorch.utils.paths",
]


@pytest.mark.parametrize("module", MODULES)
def test_module_imports(module):
    importlib.import_module(module)


def test_public_api_names_all_resolve():
    assert [name for name in startorch.__all__ if not hasattr(startorch, name)] == []


def test_works_ingestor_is_registered():
    assert EntityIngestor.registry.get("works") is WorksIngestor


def test_entity_ingestor_is_abstract():
    with pytest.raises(TypeError):
        EntityIngestor(Path("a"), Path("b"), Path("c"))  # pyright: ignore[reportAbstractUsage]


def test_abstract_inverted_index_is_not_extracted():
    assert "abstract_inverted_index" not in WorksIngestor.extracted_columns
    assert "abstract_inverted_index" not in WorksIngestor.columns


def test_works_subsetter_stores_its_configuration():
    s = WorksSubsetter(Path("full_corpus"), Path("subset"), Path("spill"), "language = 'en'")
    assert (s.full_corpus_path, s.subset_path, s.spill_path) == (Path("full_corpus"), Path("subset"), Path("spill"))
    assert s.filter_condition == "language = 'en'"
    assert s.rows_per_chunk == 2000000


@pytest.mark.parametrize("name", profile_names("openalex"))
def test_openalex_filter_is_a_valid_boolean_expression(name):
    # One synthetic row shaped like a compact Works record, so each profile's
    # filter is checked as SQL without touching real corpus data.
    con = duckdb.connect()
    con.sql("""
        CREATE TABLE _probe AS SELECT
            [{'field_display_name': 'Mathematics'}] AS topics,
            'en' AS language
    """)
    result = fetch_one(con.sql(f"SELECT ({profile(name).filter}) AS matches_filter FROM _probe"))[0]
    assert result in (True, False)


# Where the indexes, query sets and results live on disk today. A config or
# paths.py change that moves any of them fails here.
GOLDEN_PATHS = {
    ("full-en", "meta_path"): "/data/scholar_rank/posting/full-en/posting/metadata.bin",
    ("full-en", "lookup_file"): "/data/scholar_rank/posting/full-en/lookup/doc_id_lookup.bin",
    ("full-en", "token_dir"): "/data/scholar_rank/posting/full-en/token_stream",
    ("full-en", "partial_dir"): "/data/scholar_rank/posting/full-en/posting/partial",
    ("full-en", "query_dir"): "/data/scholar_rank/benchmark/data/full-en",
    ("full-en", "result_dir"): "/data/scholar_rank/benchmark/full-en/query_result",
    ("math-en", "subset_dir"): "/data/scholar_rank/data/works_subset/math-en",
    ("msmarco", "meta_path"): "/data/scholar_rank/benchmark/msmarco/posting/metadata.bin",
    ("msmarco", "token_dir"): "/data/scholar_rank/benchmark/msmarco/token_stream",
    ("msmarco", "query_dir"): "/data/scholar_rank/benchmark/data/msmarco",
    ("msmarco", "result_dir"): "/data/scholar_rank/benchmark/msmarco/query_result",
    ("msmarco", "collection"): "/data/scholar_rank/benchmark/data/msmarco/collection.tsv",
}


@pytest.mark.parametrize(("name", "attr"), GOLDEN_PATHS)
def test_profile_resolves_golden_path(name, attr):
    assert str(getattr(profile(name), attr)) == GOLDEN_PATHS[(name, attr)]


def test_profile_build_parameters_and_materialize_flags():
    assert profile("msmarco").build_params == {"k1": 0.82, "b": 0.68}
    assert not profile("full-en").materialize
    assert profile("math-en").materialize


def test_unknown_profile_raises_key_error():
    with pytest.raises(KeyError):
        profile("no-such-profile")


def test_build_index_rejects_out_of_range_parameters_before_reading_anything(tmp_path):
    with pytest.raises(ValueError):
        startorch_cpp.build_index(tmp_path / "tokens", tmp_path / "partial", tmp_path / "out", k1=0.0)
    assert not (tmp_path / "out").exists(), "nothing may be created for invalid parameters"
