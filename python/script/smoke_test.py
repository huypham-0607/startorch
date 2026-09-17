"""Preliminary smoke tests: does everything still import, wire together, and run?

Not a real test suite (no pytest dependency exists yet - see CLAUDE.md's "Python
stack refactor" notes for that as a planned, not-started goal). This is a fast,
dependency-free sanity gate for exactly the kind of breakage that's already bitten
this codebase once: a stale import (get_manifest_data moved into EntityIngestor,
left as a dangling module-level import in works_subset.py) that only surfaces at
runtime, not at a glance. Checks import-time correctness, class wiring, config
consistency, and the real CLI entry point (via subprocess, not just in-process
argparse) with --help - never touches S3, DuckDB files, or real corpus data.

Run: python python/script/smoke_test.py
"""

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PYTHON_SRC = REPO_ROOT / "python" / "src"
PYTHON_EXE = sys.executable

sys.path.insert(0, str(PYTHON_SRC))

failures = []


def check(name):
    """Decorator: run fn, record pass/fail, never let one check's exception kill the run."""
    def wrap(fn):
        try:
            fn()
            print(f"[PASS] {name}")
        except Exception as e:
            print(f"[FAIL] {name}: {type(e).__name__}: {e}")
            failures.append(name)
        return fn
    return wrap


@check("import startorch.utils")
def _():
    import startorch.utils  # noqa: F401


@check("import startorch.ingest.fetch_data")
def _():
    import startorch.ingest.fetch_data  # noqa: F401


@check("import startorch.works_subset.works_subset")
def _():
    import startorch.works_subset.works_subset  # noqa: F401


@check("import startorch.tokenizer.tokenizer")
def _():
    import startorch.tokenizer.tokenizer  # noqa: F401


@check("import startorch.cli")
def _():
    import startorch.cli  # noqa: F401


@check("EntityIngestor.registry contains 'works' -> WorksIngestor")
def _():
    from startorch.ingest.fetch_data import EntityIngestor, WorksIngestor
    assert EntityIngestor.registry.get("works") is WorksIngestor, EntityIngestor.registry


@check("EntityIngestor itself is not instantiable (still abstract)")
def _():
    from startorch.ingest.fetch_data import EntityIngestor
    try:
        EntityIngestor(Path("a"), Path("b"), Path("c"))
        raise AssertionError("EntityIngestor was instantiable - abstract methods not enforced")
    except TypeError:
        pass


@check("abstract_inverted_index removed from WorksIngestor.extracted_columns and .columns")
def _():
    from startorch.ingest.fetch_data import WorksIngestor
    assert "abstract_inverted_index" not in WorksIngestor.extracted_columns, WorksIngestor.extracted_columns
    assert "abstract_inverted_index" not in WorksIngestor.columns, WorksIngestor.columns


@check("WorksSubsetter constructs with (full_corpus_path, subset_path, spill_path, filter_condition)")
def _():
    from startorch.works_subset.works_subset import WorksSubsetter
    s = WorksSubsetter(Path("full_corpus"), Path("subset"), Path("spill"), "language = 'en'")
    assert s.full_corpus_path == Path("full_corpus")
    assert s.subset_path == Path("subset")
    assert s.spill_path == Path("spill")
    assert s.filter_condition == "language = 'en'"
    assert s.rows_per_chunk == 2000000


@check("project-config.toml's subset-profiles are all valid DuckDB boolean expressions")
def _():
    # Doesn't touch real corpus data - builds a single synthetic row shaped like a
    # compact Works record (nested topics struct + language) and evaluates each
    # profile's filter condition against it via DuckDB directly, matching exactly
    # the "(condition) AS matches_filter" expression works_subset.py now runs in
    # validate_database(). Catches SQL typos/invalid syntax in project-config.toml
    # without needing a real subset on disk.
    import duckdb
    import tomllib

    with open(REPO_ROOT / "project-config.toml", "rb") as f:
        config = tomllib.load(f)

    profiles = config["works-subset"]["subset-profiles"]
    assert len(profiles) > 0, "no subset-profiles defined"

    con = duckdb.connect()
    con.sql("""
        CREATE TABLE _probe AS SELECT
            [{'field_display_name': 'Mathematics'}] AS topics,
            'en' AS language
    """)
    for name, condition in profiles.items():
        result = con.sql(f"SELECT ({condition}) AS matches_filter FROM _probe").fetchone()[0]
        assert result in (True, False), f"profile {name!r} condition did not evaluate to a boolean: {result!r}"


def _run_cli_help(subcommand_args):
    """Invoke the real entry point (python -m startorch.cli ...) as a subprocess,
    the same way a user actually runs it - not just an in-process argparse call.
    This is what would have caught the original SyntaxError/typo bugs immediately."""
    proc = subprocess.run(
        [PYTHON_EXE, "-m", "startorch.cli", *subcommand_args, "--help"],
        cwd=str(PYTHON_SRC), capture_output=True, text=True, timeout=30,
    )
    assert proc.returncode == 0, f"exit {proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
    assert "usage:" in proc.stdout.lower(), proc.stdout


@check("CLI --help: top-level")
def _():
    _run_cli_help([])


@check("CLI --help: ingest")
def _():
    _run_cli_help(["ingest"])


@check("CLI --help: gen-works-subset")
def _():
    _run_cli_help(["gen-works-subset"])


@check("CLI gen-works-subset --profile choices match project-config.toml exactly")
def _():
    import tomllib
    with open(REPO_ROOT / "project-config.toml", "rb") as f:
        config = tomllib.load(f)
    expected = set(config["works-subset"]["subset-profiles"].keys())

    proc = subprocess.run(
        [PYTHON_EXE, "-m", "startorch.cli", "gen-works-subset", "--help"],
        cwd=str(PYTHON_SRC), capture_output=True, text=True, timeout=30,
    )
    assert proc.returncode == 0, proc.stderr
    for name in expected:
        assert name in proc.stdout, f"profile {name!r} missing from --help output:\n{proc.stdout}"


print()
if failures:
    print(f"{len(failures)} check(s) failed: {failures}")
    sys.exit(1)
else:
    print("All checks passed.")
    sys.exit(0)
