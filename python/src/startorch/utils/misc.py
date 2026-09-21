"""Small helpers with no better home.

Anything that grows a second related function belongs in its own module
instead (see logger.py, duckdb.py).

Attributes:
    PROJECT_ROOT: The repository root, which holds project-config.toml.
"""

from datetime import datetime
from pathlib import Path

# This file is <root>/python/src/startorch/utils/misc.py, so the repository
# root is five levels up. Moving this file means changing this number.
PROJECT_ROOT = Path(__file__).resolve().parents[4]


def get_current_time() -> str:
    """Returns the current local time as a human-readable string.

    Returns:
        The time formatted as "%Y-%m-%d %H:%M:%S %Z %z". The zone fields come
        out empty, since datetime.now() carries no time zone.
    """
    formatted_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S %Z %z")
    return formatted_time
