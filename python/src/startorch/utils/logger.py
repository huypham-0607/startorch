import logging
import sys


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
