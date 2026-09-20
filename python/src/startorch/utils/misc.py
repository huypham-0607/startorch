"""Small helpers with no better home. Anything that grows a second related
function belongs in its own module instead (see logger.py, duckdb.py).
"""

from datetime import datetime
from pathlib import Path

# This file is <root>/python/src/startorch/utils/misc.py, so the repository
# root is five levels up. Moving this file means changing this number.
PROJECT_ROOT = Path(__file__).resolve().parents[4]


def get_current_time() -> str:
    formatted_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S %Z %z")
    return formatted_time
