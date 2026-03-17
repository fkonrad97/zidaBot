# How-To Guide — Trading Stack (PoP + Brain)

End-to-end guide covering prerequisites, build, TLS setup, and running the stack.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Build](#2-build)
3. [TLS Certificate Setup](#3-tls-certificate-setup)
4. [Running Brain](#4-running-brain)
5. [Running PoP](#5-running-pop)
6. [End-to-End Test (Brain + 2 PoP instances)](#6-end-to-end-test-brain--2-pop-instances)
7. [Monitoring Output](#7-monitoring-output)
8. [Common Flags Reference](#8-common-flags-reference)
9. [Troubleshooting](#9-troubleshooting)

---

## 1. Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| CMake | ≥ 3.20 | `cmake --version` |
| GCC or Clang | C++23 capable | GCC ≥ 13 or Clang ≥ 17 |
| Boost | ≥ 1.89 | Beast, Asio, program_options |
| OpenSSL | any recent | For TLS |
| ZLIB | any recent | For `.gz` persistence |
| nlohmann/json | auto | Fetched by CMake FetchContent |
| openssl CLI | any | For generating dev certs |

**Ubuntu/Debian install:**
```bash
sudo apt update
sudo apt install cmake g++ libboost-all-dev libssl-dev zlib1g-dev
```

---

## 2. Build

```bash
# From the repo root
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

Outputs:
- `build/pop/pop` — market data ingestion process
- `build/brain/brain` — central arb detection server

**Build a single target:**
```bash
cmake --build build --target pop    # PoP only
cmake --build build --target brain  # Brain only
```

**Strict warnings build:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPOP_ENABLE_WERROR=ON
cmake --build build -j4
```

**AddressSanitizer + UBSan (dev/CI):**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build -j4
```

**ThreadSanitizer (dev/CI):**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build -j4
```

> ASAN and TSAN are mutually exclusive — do not enable both at once.

**After changing CMakeLists.txt or adding new source files:**
```bash
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

---

## 3. TLS Certificate Setup

Brain requires a TLS certificate. For local development, generate a self-signed cert:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout /tmp/brain_key.pem \
  -out    /tmp/brain_cert.pem \
  -subj   "/CN=localhost"
```

This creates:
- `/tmp/brain_cert.pem` — certificate (pass to `--certfile`)
- `/tmp/brain_key.pem` — private key (pass to `--keyfile`)

> **Note:** The cert is valid for 365 days. Re-run the command when it expires.

**For PoP connecting to a self-signed cert**, pass `--brain_ws_insecure` to disable TLS peer verification. This is dev-only — never use in production.

---

## 4. Running Brain

Brain must be started **before** PoP instances connect.

**Minimal (no output file):**
```bash
./build/brain/brain \
  --certfile /tmp/brain_cert.pem \
  --keyfile  /tmp/brain_key.pem
```

**With arb signal output:**
```bash
./build/brain/brain \
  --port 8443 \
  --certfile /tmp/brain_cert.pem \
  --keyfile  /tmp/brain_key.pem \
  --output   /tmp/arb.jsonl \
  --min-spread-bps 0 \
  --depth 50
```

**All options:**

| Flag | Default | Description |
|---|---|---|
| `--port` | 8443 | WSS listen port |
| `--bind` | 0.0.0.0 | Bind address |
| `--certfile` | required | TLS cert PEM |
| `--keyfile` | required | TLS key PEM |
| `--output` | (none) | Arb signal JSONL output file |
| `--min-spread-bps` | 0 | Only emit crosses above this threshold |
| `--max-age-ms` | 5000 | Staleness guard: max age of each individual book (absolute) AND max age difference between the two books (relative), both in ms |
| `--depth` | 50 | Order book depth per venue |

Brain prints detected arb crosses to **stderr** and optionally to the `--output` JSONL file. PoP connections/disconnections are also logged to stderr.

---

## 5. Running PoP

One PoP process per venue. Requires `--venue`, `--base`, and `--quote`.

**Minimal (no persistence, no brain):**
```bash
./build/pop/pop --venue binance --base BTC --quote USDT --debug
```

**With brain publishing:**
```bash
./build/pop/pop \
  --venue binance --base BTC --quote USDT \
  --brain_ws_host 127.0.0.1 \
  --brain_ws_port 8443 \
  --brain_ws_path / \
  --brain_ws_insecure \
  --persist_book_every_updates 500 \
  --persist_book_top 20 \
  --debug
```

**With file persistence only:**
```bash
./build/pop/pop \
  --venue okx --base BTC --quote USDT \
  --persist_path /tmp/okx_btc.jsonl.gz \
  --persist_book_every_updates 1000 \
  --persist_book_top 50 \
  --debug
```

**Supported venues:** `binance`, `okx`, `bybit`, `bitget`, `kucoin`

**Key flags:**

| Flag | Default | Description |
|---|---|---|
| `--venue` | required | Exchange name |
| `--base` | required | Base asset (e.g. `BTC`) |
| `--quote` | required | Quote asset (e.g. `USDT`) |
| `--depthLevel` | 400 | Local order book depth |
| `--brain_ws_host` | (none) | Brain hostname — enables publishing |
| `--brain_ws_port` | (none) | Brain port |
| `--brain_ws_path` | (none) | Brain WS path (e.g. `/`) |
| `--brain_ws_insecure` | false | Skip TLS cert check (dev only) |
| `--persist_path` | (none) | JSONL output file (`.gz` = compressed) |
| `--persist_book_every_updates` | 0 | `book_state` checkpoint frequency (0 = off) |
| `--persist_book_top` | 50 | Levels per side in `book_state` |
| `--rest_timeout_ms` | 8000 | REST snapshot request timeout in ms |
| `--log_path` | (none) | Write process log to file |
| `--debug` | false | Rate-limited book/seq debug output |

---

## 6. End-to-End Test (Brain + 2 PoP instances)

Open **three terminals** from the repo root.

**Terminal 1 — Brain**
```bash
./build/brain/brain \
  --port 8443 \
  --certfile /tmp/brain_cert.pem \
  --keyfile  /tmp/brain_key.pem \
  --output   /tmp/arb.jsonl \
  --min-spread-bps 0 \
  --depth 50
```

**Terminal 2 — Binance PoP**
```bash
./build/pop/pop \
  --venue binance --base BTC --quote USDT \
  --brain_ws_host 127.0.0.1 --brain_ws_port 8443 --brain_ws_path / \
  --brain_ws_insecure \
  --persist_book_every_updates 500 --persist_book_top 20 \
  --debug
```

**Terminal 3 — OKX PoP**
```bash
./build/pop/pop \
  --venue okx --base BTC --quote USDT \
  --brain_ws_host 127.0.0.1 --brain_ws_port 8443 --brain_ws_path / \
  --brain_ws_insecure \
  --persist_book_every_updates 500 --persist_book_top 20 \
  --debug
```

**Optional Terminal 4 — Watch arb output live**
```bash
tail -f /tmp/arb.jsonl
```

**Expected sequence of events:**
1. Brain starts and listens on port 8443.
2. Each PoP connects → brain logs `[WsSession] accepted from 127.0.0.1:...`.
3. PoP bootstraps (REST snapshot + WS sync) → logs `→ SYNCED`.
4. PoP begins sending `book_state` frames every 500 updates.
5. Once brain has ≥ 2 synced venues, arb scan runs on every incoming event.
6. Any crosses appear on brain stderr and in `/tmp/arb.jsonl`.

---

## 7. Monitoring Output

**Brain arb signal format (`/tmp/arb.jsonl`):**
```json
{
  "sell_venue": "binance",
  "buy_venue": "okx",
  "sell_bid_tick": 10523400,
  "buy_ask_tick": 10522800,
  "spread_bps": 0.57,
  "ts_detected_ns": 1741123456789000000,
  "sell_ts_book_ns": 1741123456780000000,
  "buy_ts_book_ns":  1741123456775000000
}
```

- `sell_bid_tick` / `buy_ask_tick` are integer ticks (`price × 100`)
- `spread_bps = ((sell_bid - buy_ask) / mid) × 10000`

**PoP debug output** (with `--debug`) prints a book snapshot every 200 updates:
```
[binance] SYNCED seq=12345678 bid=105234.00 ask=105235.50 ...
```

---

## 8. Common Flags Reference

### PoP quick reference

```bash
# Debug: raw WS messages truncated to 256 chars, every 100 messages
./build/pop/pop --venue bybit --base BTC --quote USDT \
  --debug --debug_raw --debug_every 100 --debug_raw_max 256

# Persist to file only (no brain)
./build/pop/pop --venue kucoin --base BTC --quote USDT \
  --persist_path /tmp/kucoin.jsonl.gz \
  --persist_book_every_updates 1000 \
  --persist_book_top 50

# Custom depth
./build/pop/pop --venue binance --base ETH --quote USDT \
  --depthLevel 100 --debug
```

### Brain quick reference

```bash
# Only emit crosses above 0.5 bps, books must be within 2s of each other
./build/brain/brain \
  --certfile /tmp/brain_cert.pem --keyfile /tmp/brain_key.pem \
  --min-spread-bps 0.5 \
  --max-age-ms 2000 \
  --output /tmp/arb.jsonl

# Deeper book (more levels for context, not used in arb scan itself)
./build/brain/brain \
  --certfile /tmp/brain_cert.pem --keyfile /tmp/brain_key.pem \
  --depth 100
```

---

## 9. Troubleshooting

**PoP connects but brain never emits crosses**
- Check that `--persist_book_every_updates` is set on PoP (non-zero). Brain relies on `book_state` events to sync; without them it stays in `WaitingSnapshot`.
- Verify both PoPs logged `→ SYNCED`. If one is stuck in `BOOTSTRAPPING` or `WAIT_REST_SNAPSHOT`, the arb scan won't start.
- Check `--max-age-ms` — if books are too stale relative to each other, crosses are suppressed.

**`[WsPublishSink] reconnecting...` loops in PoP**
- Brain may not be running or the port is wrong.
- Check that brain was started before PoP.
- Confirm `--brain_ws_port` matches `--port` on brain.

**TLS handshake failure**
- Ensure `--brain_ws_insecure` is passed to PoP when using a self-signed cert.
- If cert has expired, regenerate it (see [Section 3](#3-tls-certificate-setup)).

**`Error parsing command line` on brain**
- `--certfile` and `--keyfile` are required; the process will not start without them.

**Build fails with "Cannot find Boost"**
```bash
sudo apt install libboost-all-dev
rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release
```

**Build fails after restructuring (CMake cache stale)**
```bash
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

**PoP reconnects frequently (watchdog firing)**
- The exchange connection is dropping — normal on unstable networks.
- PoP will log `restartSync: watchdog` and reconnect automatically.
- Use `--debug` to see the reconnect sequence.
