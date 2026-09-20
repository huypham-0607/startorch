"""DuckDB helpers. `import duckdb` elsewhere still resolves to the real
package: Python 3 imports are absolute, so this module never shadows it.
"""


def fetch_one(result) -> tuple:
    """First row of a DuckDB query that always returns one, such as count(*).

    DuckDB's fetchone() returns None when there are no rows; this raises
    instead, so callers can index the row directly.
    """
    row = result.fetchone()
    if row is None:
        raise RuntimeError("Query returned no rows.")
    return row
