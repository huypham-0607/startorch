import json

import jinja2
import pandas as pd

from pathlib import Path

REPORT_DIR = Path(__file__).parent
BUILD_DIR = REPORT_DIR / "build"

LATENCY_COLS = ["mean", "p50", "p95", "p99", "max", "min"]

def ms(x):
    return f"{x:.3f}"

def to_markdown_table(records):
    df = pd.DataFrame(records)
    cols_to_format = [c for c in LATENCY_COLS if c in df.columns]
    if cols_to_format:
        df = df.assign(**{c: df[c].map(ms) for c in cols_to_format})
    return df.to_markdown(index=False)

data = json.loads((BUILD_DIR / "agg.json").read_text())

tables = {
    "msmarco": to_markdown_table(data["summary_msmarco"]),
    "openalex": to_markdown_table(data["summary_openalex"]),
    "rank_safety": to_markdown_table(data["summary_rank_safety"]),
}

env = jinja2.Environment(
    loader=jinja2.FileSystemLoader(REPORT_DIR / "templates"),
    undefined=jinja2.StrictUndefined,
)
env.filters["ms"] = ms

report = env.get_template("report.md.j2").render(tables=tables, **data)

out_path = BUILD_DIR / "report.md"
out_path.write_text(report)

print(f"Wrote {out_path}")
