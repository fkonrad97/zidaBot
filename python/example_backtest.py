"""
example_backtest.py — Replay historical PoP JSONL files through the C++ arb engine.

Build zidabot_replay first:
    cmake --build build -j4 --target zidabot_replay

Usage:
    python3 python/example_backtest.py \\
        --binary build/brain/zidabot_replay \\
        persist/pop-data/2026-03-05-20-27-39/binance.jsonl.gz \\
        persist/pop-data/2026-03-05-20-27-39/okx.jsonl.gz
"""

import argparse
import gzip
import json
import subprocess
import sys
from dataclasses import dataclass
from typing import Iterator


@dataclass
class ArbCross:
    sell_venue:      str
    buy_venue:       str
    sell_bid_tick:   int
    buy_ask_tick:    int
    spread_bps:      float
    ts_detected_ns:  int
    sell_ts_book_ns: int
    buy_ts_book_ns:  int
    lag_ns:          int


def iter_lines(path: str) -> Iterator[str]:
    """Yield stripped lines from a plain or gzip-compressed JSONL file."""
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt") as f:
        for line in f:
            line = line.strip()
            if line:
                yield line


def replay(files: list[str], binary: str = "./build/brain/zidabot_replay",
           **engine_kwargs) -> list[ArbCross]:
    """
    Replay JSONL files through the C++ BacktestEngine subprocess.

    engine_kwargs are forwarded as CLI flags:
        depth, min_spread_bps, max_spread_bps, rate_limit_ms,
        max_age_ms, max_price_deviation_pct
    """
    cmd = [binary]
    flag_map = {
        "depth":                   "--depth",
        "min_spread_bps":          "--min-spread-bps",
        "max_spread_bps":          "--max-spread-bps",
        "rate_limit_ms":           "--rate-limit-ms",
        "max_age_ms":              "--max-age-ms",
        "max_price_deviation_pct": "--max-price-deviation-pct",
    }
    for key, flag in flag_map.items():
        if key in engine_kwargs:
            cmd += [flag, str(engine_kwargs[key])]

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    results: list[ArbCross] = []

    try:
        for path in files:
            print(f"  replaying {path} ...", file=sys.stderr)
            for line in iter_lines(path):
                proc.stdin.write(line + "\n")
                proc.stdin.flush()
                out = proc.stdout.readline()
                if out.strip():
                    d = json.loads(out)
                    results.append(ArbCross(**d))
    finally:
        proc.stdin.close()
        proc.wait()

    return results


def main() -> None:
    parser = argparse.ArgumentParser(description="Replay JSONL files through the C++ arb engine")
    parser.add_argument("files", nargs="+", help="JSONL or .jsonl.gz files to replay")
    parser.add_argument("--binary",                  default="./build/brain/zidabot_replay")
    parser.add_argument("--depth",                   type=int,   default=50)
    parser.add_argument("--min-spread-bps",          type=float, default=0.0, dest="min_spread_bps")
    parser.add_argument("--max-spread-bps",          type=float, default=0.0, dest="max_spread_bps")
    parser.add_argument("--rate-limit-ms",           type=int,   default=0,   dest="rate_limit_ms")
    parser.add_argument("--max-age-ms",              type=int,   default=5000, dest="max_age_ms")
    parser.add_argument("--max-price-deviation-pct", type=float, default=0.0, dest="max_price_deviation_pct")
    args = parser.parse_args()

    crosses = replay(
        args.files,
        binary=args.binary,
        depth=args.depth,
        min_spread_bps=args.min_spread_bps,
        max_spread_bps=args.max_spread_bps,
        rate_limit_ms=args.rate_limit_ms,
        max_age_ms=args.max_age_ms,
        max_price_deviation_pct=args.max_price_deviation_pct,
    )

    print(f"\nTotal crosses detected: {len(crosses)}", file=sys.stderr)

    if not crosses:
        print("No crosses detected.")
        return

    try:
        import pandas as pd
        df = pd.DataFrame([vars(c) for c in crosses])
        print(df.to_string(index=False))
        print("\n--- Summary by venue pair ---")
        print(df.groupby(["sell_venue", "buy_venue"])["spread_bps"].describe().to_string())
    except ImportError:
        for c in crosses[:20]:
            print(c)
        if len(crosses) > 20:
            print(f"... and {len(crosses) - 20} more")


if __name__ == "__main__":
    main()
