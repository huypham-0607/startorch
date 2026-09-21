"""API Endpoint using FastAPI

"""

from startorch.utils.paths import profile
from startorch.lexical.search import Searcher


from contextlib import asynccontextmanager
from fastapi import FastAPI

PROFILE = "msmarco"

resources = {}

@asynccontextmanager
async def lifespan(app: FastAPI):
    resources["searcher"] = Searcher(profile(PROFILE))

    yield

app = FastAPI(lifespan = lifespan)

@app.get("/")
async def root():
    return {"message": "Hello World"}


@app.get("/search/")
async def read_item(query: str, k: int):
    return resources["searcher"].search(query, k)