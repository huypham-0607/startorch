import logging
import sys

from datetime import datetime
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent.parent.parent

def get_logger(name: str) -> logging.Logger:
    """Formatting default logger for general use cases.

    Args:
        name: Name of logger (typically __name__)

    Returns:
        logging.Logger object with customized formatter.

    """
    logger = logging.getLogger(name)
    if not logger.handlers:
        handler = logging.StreamHandler(sys.stdout)
        handler.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(name)s: %(message)s"))
        logger.addHandler(handler)
    logger.setLevel(logging.INFO) # Set min displayed warning level
    return logger

def get_current_time() -> str:
    formatted_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S %Z %z")
    return formatted_time

def fetch_one(result) -> tuple:
    """First row of a DuckDB query that always returns one, such as count(*).

    DuckDB's fetchone() returns None when there are no rows; this raises
    instead, so callers can index the row directly.
    """
    row = result.fetchone()
    if row is None:
        raise RuntimeError("Query returned no rows.")
    return row

