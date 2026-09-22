"""Request and response shapes of the HTTP API.

These are the API's public contract. They are kept separate from
startorch.lexical, so a change inside Searcher never silently changes what
clients receive.
"""

from pydantic import BaseModel, Field


class Hit(BaseModel):
    """One search result.

    Attributes:
        id: The document id. OpenAlex works carry their "W" prefix, so the id can
            be passed straight to the OpenAlex API; MS MARCO ids are the pid.
        score: The BM25 score. Higher is better.
    """
    id: str = Field(description='Document id, e.g. "W2626778328", or an MS MARCO pid.')
    score: float = Field(description="BM25 score. Higher is better.")


class SearchResponse(BaseModel):
    """The response to one search.

    Attributes:
        query: The query as received.
        k: The number of results requested.
        took_ms: C++ search time in milliseconds, excluding tokenization and id mapping.
        hits: Up to k results, best first. Empty if no document matches.
    """
    query: str
    k: int
    took_ms: float = Field(description="C++ search time only, in milliseconds.")
    hits: list[Hit]
