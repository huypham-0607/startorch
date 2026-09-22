"""The real `startorch` entry point, run as a subprocess the way a user runs it.

A subprocess catches what an in-process argparse call would not: import-time
errors in any module the CLI pulls in.
"""

import re
import subprocess
import sys

import pytest

from startorch.utils.paths import profile_names


def run_help(*args: str) -> str:
    """Runs `python -m startorch.cli <args> --help` and returns its stdout."""
    proc = subprocess.run(
        [sys.executable, "-m", "startorch.cli", *args, "--help"],
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 0, f"exit {proc.returncode}\n{proc.stdout}\n{proc.stderr}"
    return proc.stdout


@pytest.mark.parametrize("command", [[], ["ingest"], ["gen-works-subset"], ["build-posting"], ["query"]])
def test_help_runs(command):
    assert "usage:" in run_help(*command).lower()


@pytest.mark.parametrize(("command", "kind"), [("gen-works-subset", "openalex"), ("build-posting", None), ("query", None)])
def test_profile_choices_match_config(command, kind):
    match = re.search(r"--profile \{([^}]*)\}", run_help(command))
    assert match, "no --profile choices in --help"
    assert set(match.group(1).split(",")) == set(profile_names(kind))
