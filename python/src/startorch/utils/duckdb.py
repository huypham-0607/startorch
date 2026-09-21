"""Helpers for working with DuckDB query results.

`import duckdb` elsewhere still resolves to the real package: Python 3 imports
are absolute, so this module never shadows it.
"""


def fetch_one(result) -> tuple:
    """Returns the first row of a query that always produces one, such as count(*).

    DuckDB's fetchone() returns None when there are no rows; this raises
    instead, so callers can index the row directly.

    Args:
        result: A DuckDB relation, or a connection with a pending result.

    Returns:
        The first row, as a tuple.

    Raises:
        RuntimeError: If the query returned no rows.
    """
    row = result.fetchone()
    if row is None:
        raise RuntimeError("Query returned no rows.")
    return row
