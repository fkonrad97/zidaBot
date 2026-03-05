#!/usr/bin/env python3
"""Streamlit UI for replaying cached composite order book pages."""

from __future__ import annotations

from pathlib import Path

import pandas as pd
import streamlit as st


DEFAULT_CACHE_PATH = Path(
    "/home/fkonrad97/projects/pop/persist/analysis-cache/composite_pages_btcusdt.pkl.gz"
)


@st.cache_data(show_spinner="Loading cached composite pages...")
def load_pages(cache_path: str) -> pd.DataFrame:
    """Load cached composite pages from a gzip-compressed pickle."""
    return pd.read_pickle(cache_path, compression="gzip")


def page_table(records: list[dict], depth: int) -> pd.DataFrame:
    """Convert one nested page payload into a DataFrame and trim depth."""
    if not records:
        return pd.DataFrame()
    return pd.DataFrame(records).head(depth)


def main() -> None:
    st.set_page_config(page_title="Composite Order Book Replay", layout="wide")
    st.title("Composite Order Book Replay")
    st.caption("Replay cached unified order book pages without recomputing the source replay.")

    cache_path = st.sidebar.text_input("Cache path", str(DEFAULT_CACHE_PATH))
    cache_file = Path(cache_path)

    if not cache_file.exists():
        st.error(f"Cache file not found: {cache_file}")
        st.info("Generate the cache first from scripts/composite_orderbook.ipynb.")
        return

    try:
        pages_df = load_pages(str(cache_file))
    except Exception as exc:  # pragma: no cover - UI error path
        st.exception(exc)
        return

    if pages_df.empty:
        st.warning("Cached pages_df is empty.")
        return

    cache_size_mb = cache_file.stat().st_size / (1024 * 1024)
    st.subheader("Cache Status")
    s1, s2, s3 = st.columns(3)
    s1.metric("Rows", int(len(pages_df)))
    s2.metric("Cache Size (MB)", f"{cache_size_mb:.2f}")
    s3.metric("Max Active Venues", int(pages_df["venue_count"].max()) if "venue_count" in pages_df else 0)
    st.code(str(cache_file), language="text")

    max_index = len(pages_df) - 1
    depth = st.sidebar.selectbox("Depth", [5, 10, 20, 50], index=1)
    page_index = st.sidebar.slider("Page", min_value=0, max_value=max_index, value=max_index)

    chart_window = st.sidebar.slider(
        "Chart window",
        min_value=50,
        max_value=min(max(50, len(pages_df)), 5000),
        value=min(500, len(pages_df)),
        step=50,
    )

    page = pages_df.iloc[page_index]

    st.subheader("Page Summary")
    c1, c2, c3, c4 = st.columns(4)
    c1.metric("Page", int(page_index))
    c2.metric("Timestamp", str(page["ts_book_dt"]))
    c3.metric("Venue Count", int(page["venue_count"]))
    spread_value = page["spread"]
    c4.metric("Spread", "n/a" if pd.isna(spread_value) else f"{float(spread_value):.8f}")

    st.json(
        {
            "ts_book_ns": int(page["ts_book_ns"]),
            "venues": page["venues"],
            "best_bid": None if pd.isna(page["best_bid"]) else float(page["best_bid"]),
            "best_ask": None if pd.isna(page["best_ask"]) else float(page["best_ask"]),
            "spread": None if pd.isna(page["spread"]) else float(page["spread"]),
        }
    )

    st.subheader("Venue Books")
    venue_books_df = pd.DataFrame(page["venue_books"])
    st.dataframe(venue_books_df, use_container_width=True)

    st.subheader("Unified Order Book")
    col_bids, col_asks = st.columns(2)
    with col_bids:
        st.markdown("**Bids**")
        st.dataframe(page_table(page["unified_bids"], depth), use_container_width=True)
    with col_asks:
        st.markdown("**Asks**")
        st.dataframe(page_table(page["unified_asks"], depth), use_container_width=True)

    st.subheader("Replay Chart")
    window_start = max(0, page_index - chart_window + 1)
    chart_df = pages_df.iloc[window_start : page_index + 1][
        ["ts_book_dt", "best_bid", "best_ask", "spread"]
    ].copy()
    chart_df = chart_df.set_index("ts_book_dt")
    st.line_chart(chart_df, height=320)


if __name__ == "__main__":
    main()
