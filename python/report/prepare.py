import json
import statistics

import numpy as np
import pandas as pd

from pathlib import Path
from startorch import profile

msmarco_bench_path = profile("msmarco").result_dir / "perf_raw.parquet"
openalex_bench_path = profile("full-en").result_dir / "perf_raw.parquet"
correctness_bench_path = profile("full-en").result_dir / "correctness_raw.parquet"

msmarco_bench = pd.read_parquet(msmarco_bench_path)
openalex_bench = pd.read_parquet(openalex_bench_path)
correctness_bench = pd.read_parquet(correctness_bench_path)

def stats(perf):
    return pd.DataFrame({
        "name": perf["run_name"],
        "n": perf["query_latency"].apply(lambda x: len(x)),
        "max_rss": perf["max_rss"].astype(float).apply(lambda x: x / 1024),
        "mean": perf["query_latency"].apply(lambda x: sum(x) / len(x) if len(x) > 0 else 0),
        "p50": perf["query_latency"].apply(lambda x: statistics.median(x) if len(x) > 0 else 0),
        "p95": perf["query_latency"].apply(lambda x: np.percentile(x, 95) if len(x) > 0 else 0),
        "p99": perf["query_latency"].apply(lambda x: np.percentile(x, 99) if len(x) > 0 else 0),
        "max": perf["query_latency"].apply(lambda x: np.max(x) if len(x) > 0 else 0),
        "min": perf["query_latency"].apply(lambda x: np.min(x) if len(x) > 0 else 0),
    })

def stats_rank(correctness):
    return pd.DataFrame({
        "name": correctness["run_name"],
        "n": correctness["rank_matches"].apply(lambda x: len(x)),
        "exact_matches": correctness["query_matches"],
        "sum_missing": correctness["missing_docids"].apply(lambda per_query: sum(len(q) for q in per_query)),
        "sum_extra": correctness["extra_docids"].apply(lambda per_query: sum(len(q) for q in per_query)),
    })

summary_msmarco = stats(msmarco_bench)
summary_openalex = stats(openalex_bench)
summary_rank_safety = stats_rank(correctness_bench)

out = {
    "summary_msmarco": summary_msmarco.to_dict("records"),
    "summary_openalex": summary_openalex.to_dict("records"),
    "summary_rank_safety": summary_rank_safety.to_dict("records"),
}

build_dir = Path(__file__).parent / "build"
build_dir.mkdir(parents=True, exist_ok=True)

out_path = build_dir / "agg.json"
out_path.write_text(json.dumps(out, indent=2))

print(f"Wrote {out_path}")
