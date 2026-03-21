# How-To Guide — Trading Stack (PoP + Brain)

End-to-end guide covering prerequisites, build, TLS/mTLS setup, config files, and running the stack.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Build](#2-build)
3. [TLS & mTLS Certificate Setup](#3-tls--mtls-certificate-setup)
4. [Config File Usage](#4-config-file-usage)
5. [Running Brain](#5-running-brain)
6. [Running PoP](#6-running-pop)
7. [End-to-End Test (Brain + all 5 PoP instances)](#7-end-to-end-test-brain--all-5-pop-instances)
8. [Monitoring Output](#8-monitoring-output)
9. [Common Flags Reference](#9-common-flags-reference)
10. [Troubleshooting](#10-troubleshooting)

---

## 1. Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| CMake | ≥ 3.20 | `cmake --version` |
| GCC or Clang | C++23 capable | GCC ≥ 13 or Clang ≥ 17 |
| Boost | ≥ 1.73 | Beast, Asio, program_options (`ssl::host_name_verification` requires 1.73+) |
| OpenSSL | any recent | For TLS/mTLS |
| ZLIB | any recent | For `.gz` persistence |
| nlohmann/json | auto | Fetched by CMake FetchContent |
| spdlog | auto | Fetched by CMake FetchContent |
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

## 3. TLS & mTLS Certificate Setup

Brain always requires a TLS server certificate. Optionally, mTLS can be enabled so that Brain also verifies that each connecting PoP presents a certificate signed by a trusted CA — preventing unauthorized processes from injecting market data events.

### 3a. mTLS Setup (recommended)

Run once. Keep `ca_key.pem` secret; the other files are safe to distribute within your deployment.

```bash
mkdir -p /tmp/certs

# 1. Create a private CA (one-time)
openssl genrsa -out /tmp/certs/ca_key.pem 4096
openssl req -x509 -new -key /tmp/certs/ca_key.pem -days 3650 \
  -out /tmp/certs/ca_cert.pem -subj "/CN=zidabot-ca"

# 2. Brain TLS server certificate (signed by the CA)
openssl genrsa -out /tmp/certs/brain_key.pem 2048
openssl req -new -key /tmp/certs/brain_key.pem \
  -out /tmp/certs/brain.csr -subj "/CN=localhost"
openssl x509 -req -in /tmp/certs/brain.csr \
  -CA /tmp/certs/ca_cert.pem -CAkey /tmp/certs/ca_key.pem \
  -CAcreateserial -days 365 -out /tmp/certs/brain_cert.pem

# 3. PoP client certificate (one shared cert for all PoP instances)
openssl genrsa -out /tmp/certs/pop_key.pem 2048
openssl req -new -key /tmp/certs/pop_key.pem \
  -out /tmp/certs/pop.csr -subj "/CN=pop-client"
openssl x509 -req -in /tmp/certs/pop.csr \
  -CA /tmp/certs/ca_cert.pem -CAkey /tmp/certs/ca_key.pem \
  -CAcreateserial -days 365 -out /tmp/certs/pop_cert.pem
```

This produces:
| File | Purpose |
|---|---|
| `/tmp/certs/ca_cert.pem` | CA certificate — pass to brain `--ca-certfile` |
| `/tmp/certs/ca_key.pem` | CA private key — keep secret, not passed to any binary |
| `/tmp/certs/brain_cert.pem` | Brain TLS server cert — pass to brain `--certfile` |
| `/tmp/certs/brain_key.pem` | Brain TLS server key — pass to brain `--keyfile` |
| `/tmp/certs/pop_cert.pem` | PoP client cert — pass to each PoP `--brain_ws_certfile` |
| `/tmp/certs/pop_key.pem` | PoP client key — pass to each PoP `--brain_ws_keyfile` |

The ready-to-use `config/` files already reference these paths.

> **Cert expiry:** Certs above are valid for 365 days. Re-run steps 2–3 (not step 1) when they expire. The CA itself is valid for 10 years.

### 3b. Dev-only: self-signed cert without mTLS

For quick local testing without client certs:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout /tmp/brain_key.pem \
  -out    /tmp/brain_cert.pem \
  -subj   "/CN=localhost"
```

Then pass `--brain_ws_insecure` to each PoP to skip TLS peer verification, and omit `--ca-certfile` from Brain. **Never use `--brain_ws_insecure` in production.**

---

## 4. Config File Usage

Both `pop` and `brain` accept a `--config FILE` flag that reads settings from a plain-text file, eliminating long command lines for multi-venue deployments.

### Format

```ini
# Lines starting with # are comments
key=value          # key names are identical to CLI flag names, without the leading --
key2=true          # boolean flags use true/false
```

**Key names:** same as the CLI flag name, without `--`. For example, `--brain_ws_host` becomes `brain_ws_host=`.

**Override precedence:** CLI arguments always win over the config file. This means you can keep a shared config file and override individual settings per-run:

```bash
# Use the binance config but raise log level for this run
./build/pop/pop --config config/binance.conf --log_level debug

# Override cert paths without editing the file
./build/brain/brain --config config/brain.conf \
  --certfile /etc/ssl/brain_cert.pem \
  --keyfile  /etc/ssl/brain_key.pem
```

### Ready-to-use config files

The `config/` directory in this repo contains one file per process:

| File | Process |
|---|---|
| `config/brain.conf` | Brain |
| `config/binance.conf` | PoP — Binance BTC/USDT |
| `config/okx.conf` | PoP — OKX BTC/USDT |
| `config/bybit.conf` | PoP — Bybit BTC/USDT |
| `config/bitget.conf` | PoP — Bitget BTC/USDT |
| `config/kucoin.conf` | PoP — KuCoin BTC/USDT |

All files assume certs are in `/tmp/certs/` (see [Section 3a](#3a-mtls-setup-recommended)). Edit the paths if your certs live elsewhere.

---

## 5. Running Brain

Brain must be started **before** PoP instances connect.

**With config file (recommended):**
```bash
./build/brain/brain --config config/brain.conf
```

**Minimal (no config file, no output file):**
```bash
./build/brain/brain \
  --certfile /tmp/certs/brain_cert.pem \
  --keyfile  /tmp/certs/brain_key.pem
```

**With mTLS + arb output:**
```bash
./build/brain/brain \
  --certfile    /tmp/certs/brain_cert.pem \
  --keyfile     /tmp/certs/brain_key.pem \
  --ca-certfile /tmp/certs/ca_cert.pem \
  --port 8443 \
  --output /tmp/arb.jsonl \
  --min-spread-bps 0 \
  --depth 50
```

**All options:**

| Flag | Default | Description |
|---|---|---|
| `--port` | 8443 | WSS listen port |
| `--bind` | 0.0.0.0 | Bind address |
| `--certfile` | required | TLS server cert PEM |
| `--keyfile` | required | TLS server key PEM |
| `--ca-certfile` | (none) | mTLS: CA cert PEM — require PoP clients to present a cert signed by this CA |
| `--output` | (none) | Arb signal JSONL output file |
| `--min-spread-bps` | 0 | Only emit crosses at or above this threshold in bps (0 = all) |
| `--max-spread-bps` | 0 | Suppress crosses above this threshold (0 = no cap); logged as anomalies |
| `--rate-limit-ms` | 1000 | Suppress repeated signals for the same (sell, buy) pair within this window (ms) |
| `--max-age-ms` | 5000 | Staleness guard: max individual book age AND max age diff between the two books (ms) |
| `--max-price-deviation-pct` | 0 | Skip venues deviating > N% from median best_bid across all synced venues (0 = off) |
| `--depth` | 50 | Order book depth per venue |
| `--output-max-mb` | 0 | Rotate `--output` at this file size in MB (0 = no rotation) |
| `--watchdog-no-cross-sec` | 0 | Warn if no cross emitted for N seconds while ≥ 2 venues synced (0 = off) |
| `--log-level` | info | Log verbosity: `debug` \| `info` \| `warn` \| `error` |
| `--config` | (none) | Config file path (key=value per line; CLI flags override file values) |

Brain prints detected arb crosses to **stderr** and optionally to the `--output` JSONL file. PoP connections/disconnections are also logged to stderr.

---

## 6. Running PoP

One PoP process per venue. Requires `--venue`, `--base`, and `--quote`.

**With config file (recommended):**
```bash
./build/pop/pop --config config/binance.conf
```

**Minimal (no persistence, no brain):**
```bash
./build/pop/pop --venue binance --base BTC --quote USDT --debug
```

**With brain + mTLS:**
```bash
./build/pop/pop \
  --venue binance --base BTC --quote USDT \
  --brain_ws_host 127.0.0.1 \
  --brain_ws_port 8443 \
  --brain_ws_path / \
  --brain_ws_certfile /tmp/certs/pop_cert.pem \
  --brain_ws_keyfile  /tmp/certs/pop_key.pem \
  --persist_book_every_updates 500 \
  --persist_book_top 20
```

**With file persistence only (no brain):**
```bash
./build/pop/pop \
  --venue okx --base BTC --quote USDT \
  --persist_path /tmp/okx_btc.jsonl.gz \
  --persist_book_every_updates 1000 \
  --persist_book_top 50
```

**Supported venues:** `binance`, `okx`, `bybit`, `bitget`, `kucoin`

**Key flags:**

| Flag | Default | Description |
|---|---|---|
| `--venue` | required | Exchange name |
| `--base` | required | Base asset (e.g. `BTC`) |
| `--quote` | required | Quote asset (e.g. `USDT`) |
| `--depthLevel` | 400 | Local order book depth (Bybit caps to nearest of 1/50/200 automatically) |
| `--brain_ws_host` | (none) | Brain hostname — enables publishing |
| `--brain_ws_port` | (none) | Brain port |
| `--brain_ws_path` | (none) | Brain WS path (e.g. `/`) |
| `--brain_ws_insecure` | false | Skip TLS cert check — dev only, never use in production |
| `--brain_ws_certfile` | (none) | mTLS client cert PEM for PoP→brain connection |
| `--brain_ws_keyfile` | (none) | mTLS client key PEM for PoP→brain connection |
| `--persist_path` | (none) | JSONL output file (`.gz` = compressed) |
| `--persist_book_every_updates` | 0 | `book_state` checkpoint frequency (0 = off) |
| `--persist_book_top` | 50 | Levels per side in `book_state` |
| `--rest_timeout_ms` | 8000 | REST snapshot request timeout in ms |
| `--max_msg_rate` | 0 | Warn if msgs/sec > 2× this value at heartbeat (0 = off) |
| `--validate_every` | 0 | Periodic `OrderBook::validate()` every N updates; resync on failure (0 = off) |
| `--require_checksum` | false | Resync if checksum absent on a checksum-capable venue (OKX only currently active) |
| `--log_level` | info | Log verbosity: `debug` \| `info` \| `warn` \| `error` |
| `--log_path` | (none) | Write log to file in addition to stderr |
| `--config` | (none) | Config file path (key=value per line; CLI flags override file values) |
| `--debug` | false | Rate-limited book/seq debug output |

---

## 7. End-to-End Test (Brain + all 5 PoP instances)

**Prerequisites:** build complete, certs generated in `/tmp/certs/` ([Section 3a](#3a-mtls-setup-recommended)).

Open six terminals from the repo root (or use a terminal multiplexer like tmux).

**Terminal 1 — Brain**
```bash
./build/brain/brain --config config/brain.conf
```

**Terminal 2 — Binance**
```bash
./build/pop/pop --config config/binance.conf
```

**Terminal 3 — OKX**
```bash
./build/pop/pop --config config/okx.conf
```

**Terminal 4 — Bybit**
```bash
./build/pop/pop --config config/bybit.conf
```

**Terminal 5 — Bitget**
```bash
./build/pop/pop --config config/bitget.conf
```

**Terminal 6 — KuCoin**
```bash
./build/pop/pop --config config/kucoin.conf
```

**Watch arb output live**
```bash
tail -f /tmp/arb.jsonl
```

**Dev mode (no mTLS, no client certs):** Edit each PoP config to replace the `brain_ws_certfile` / `brain_ws_keyfile` lines with `brain_ws_insecure=true`, and comment out `ca-certfile` in `brain.conf`.

**Expected sequence of events:**
1. Brain starts and listens on `0.0.0.0:8443`.
2. Each PoP connects → brain logs `[WsServer] client connected addr=127.0.0.1`.
3. mTLS handshake succeeds (if enabled) — brain verifies PoP client cert against CA.
4. PoP bootstraps → logs `state … -> SYNCED venue=<venue>`.
5. PoP sends `book_state` frames every 500 updates; brain transitions venue to synced.
6. Once brain has ≥ 2 synced venues, arb scan runs on every incoming event.
7. Crosses appear on brain stderr and in `/tmp/arb.jsonl`.
8. Brain watchdog fires every 60 s; warns if `synced_count == 0` or no cross for 120 s.

---

## 8. Monitoring Output

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

**PoP heartbeat** (logged every 60 s at `info` level):
```
[HEARTBEAT] venue=binance msgs=12450 book_updates=11200 resyncs=0
```

**PoP debug output** (with `--log_level debug`) prints per-update sequence traces:
```
[binance] SYNCED seq=12345678 bid=105234.00 ask=105235.50 ...
```

---

## 9. Common Flags Reference

### PoP quick reference

```bash
# Debug: raw WS messages truncated to 256 chars, every 100 messages
./build/pop/pop --venue bybit --base BTC --quote USDT \
  --debug --debug_raw --debug_every 100 --debug_raw_max 256

# Config file + CLI override (raise log level for one run)
./build/pop/pop --config config/binance.conf --log_level debug

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
# Config file + CLI override
./build/brain/brain --config config/brain.conf --log-level debug

# Only emit crosses above 0.5 bps, books must be within 2s of each other
./build/brain/brain \
  --certfile /tmp/certs/brain_cert.pem --keyfile /tmp/certs/brain_key.pem \
  --min-spread-bps 0.5 \
  --max-age-ms 2000 \
  --output /tmp/arb.jsonl

# With anomaly suppression + rate limit + cross-venue price guard
./build/brain/brain \
  --certfile /tmp/certs/brain_cert.pem --keyfile /tmp/certs/brain_key.pem \
  --min-spread-bps 0 --max-spread-bps 50 \
  --rate-limit-ms 500 \
  --max-price-deviation-pct 1.0 \
  --output /tmp/arb.jsonl

# File rotation + watchdog
./build/brain/brain \
  --certfile /tmp/certs/brain_cert.pem --keyfile /tmp/certs/brain_key.pem \
  --output /tmp/arb.jsonl \
  --output-max-mb 100 \
  --watchdog-no-cross-sec 60

# Deeper book
./build/brain/brain \
  --certfile /tmp/certs/brain_cert.pem --keyfile /tmp/certs/brain_key.pem \
  --depth 100
```

---

## 10. Troubleshooting

**PoP connects but brain never emits crosses**
- Check that `--persist_book_every_updates` is set on PoP (non-zero). Brain relies on `book_state` events to sync; without them it stays in `WaitingSnapshot`.
- Verify both PoPs logged `→ SYNCED`. If one is stuck in `BOOTSTRAPPING` or `WAIT_REST_SNAPSHOT`, the arb scan won't start.
- Check `--max-age-ms` — if books are too stale relative to each other, crosses are suppressed.

**`[WsPublishSink] reconnecting...` loops in PoP**
- Brain may not be running or the port is wrong.
- Check that brain was started before PoP.
- Confirm `--brain_ws_port` matches `--port` on brain.

**TLS handshake failure (self-signed cert)**
- Ensure `--brain_ws_insecure` is passed to PoP when using a self-signed cert not in the system trust store.
- If cert has expired, regenerate it (see [Section 3](#3-tls--mtls-certificate-setup)).

**mTLS handshake failure — "peer did not return a certificate"**
- Brain is requiring a client cert but PoP did not present one.
- Check that the PoP config has `brain_ws_certfile` and `brain_ws_keyfile` set to valid PEM files signed by the same CA passed to brain's `--ca-certfile`.
- Verify the client cert is not expired: `openssl x509 -in /tmp/certs/pop_cert.pem -noout -dates`

**mTLS handshake failure — "certificate verify failed"**
- The PoP client cert was not signed by the CA that brain loaded via `--ca-certfile`.
- Re-sign the PoP cert using the CA key: repeat step 3 in [Section 3a](#3a-mtls-setup-recommended).

**Config file setting seems ignored**
- `boost::parse_config_file` silently ignores unknown keys. Verify the key name matches the CLI flag name exactly (without `--`) and with the correct separator (underscore vs hyphen).
- Note: brain uses `log-level` (hyphen); PoP uses `log_level` (underscore). This difference exists in the binary's flag definitions.

**`--log_level` / `--log-level` not changing output**
- Brain uses `log-level` (hyphen); PoP uses `log_level` (underscore). Use the correct form for each binary, both in CLI and in config files.

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
- PoP will log `restartSync: watchdog` and reconnect automatically with exponential backoff (1 s → 60 s, ±25% jitter).
- Use `--log_level debug` to see the reconnect sequence.

**Bybit PoP connects but no book messages arrive**
- Bybit spot WebSocket only supports depths **1, 50, or 200**. The adapter automatically caps `--depthLevel` to the nearest valid value.
- Verify the subscribe message in the log: it should say `orderbook.50.BTCUSDT` (or `1`/`200`), never an arbitrary depth.

**Bitget PoP enters a resync loop (`steady_state_sequence_gap` or `snapshot_missing_checksum`)**
- Bitget's WS snapshot is prepared at subscribe-time; by the time the handler processes it, the live stream has typically advanced by hundreds of updates. This is expected — `allow_seq_gap=true` is set for Bitget and handles this.
- CRC-32 checksum validation is intentionally disabled for Bitget because the unverified baseline makes incremental checksums unreliable. Structural integrity is maintained by the crossed-book guard (C1) and periodic validate (C3).
- If you see a resync loop, check whether the `BitgetAdapter::caps()` still has `has_checksum=false` and `allow_seq_gap=true`.
