"""Logger factory shared by every module in the package."""

import logging
import sys


def get_logger(name: str) -> logging.Logger:
    """Returns a logger that writes timestamped INFO-level lines to stdout.

    The handler is attached only once per name, so calling this repeatedly
    (for example at import time in several modules) never duplicates output.

    Args:
        name: The logger's name, normally the calling module's __name__.

    Returns:
        The configured logging.Logger.
    """
    logger = logging.getLogger(name)
    if not logger.handlers:
        handler = logging.StreamHandler(sys.stdout)
        handler.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(name)s: %(message)s"))
        logger.addHandler(handler)
    logger.setLevel(logging.INFO) # Set min displayed warning level
    return logger
