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
11. [Health Endpoint](#11-health-endpoint)
12. [Production Deployment (systemd / supervisord)](#12-production-deployment-systemd--supervisord)
13. [Backtesting with zidabot_replay](#13-backtesting-with-zidabot_replay)
14. [Execution Safety and Venue Rollout](#14-execution-safety-and-venue-rollout)

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
| `--health-port` | 0 | D5: Plain-HTTP health endpoint port (0 = disabled). `GET /health` returns JSON. |
| `--standby` | false | F4: Passive standby mode — receives data, emits no signals. Promote with `kill -USR1 <pid>`. |
| `--config` | (none) | Config file path (key=value per line; CLI flags override file values) |

Brain prints detected arb crosses to **stderr** and optionally to the `--output` JSONL file. PoP connections/disconnections are also logged to stderr.

### Fee-aware spread config

`brain` now supports `--venue-fees` so the detector can subtract per-venue taker fees before applying `min-spread-bps`.

Example:
```bash
./build/brain/brain --config config/brain.conf \
  --venue-fees binance:10,okx:8,bybit:10
```

Important behavior:
- Whitespace is trimmed, so `binance: 10` is accepted.
- Malformed tokens are skipped with a warning instead of failing silently.
- Negative fees are rejected.
- Very large fees are accepted with a warning so operator mistakes are visible.
- If `min-spread-bps > max-spread-bps`, brain logs a warning because no crosses will emit.

---

## 6. Running PoP

One PoP process per venue. Requires `--venue` and either `--symbols` or `--base`+`--quote`.

**With config file (recommended):**
```bash
./build/pop/pop --config config/binance.conf
```

---

## 14. Execution Safety and Venue Rollout

This section covers the exec-side changes added after the initial execution-layer MVP. The short version is:

- `exec` is now **safe by default**
- dry-run is the normal first step
- live mode requires several explicit acknowledgements and size limits

### 14a. What changed

The exec pipeline now has four important safety improvements:

1. **Live fills now update E1 correctly**
   `ImmediateStrategy` forwards confirmed fills into `ExecEngine::on_fill()`, so the position-limit guard is no longer a unit-test-only behavior.

2. **Dry-run mode**
   `DryRunOrderClient` logs orders and synthesizes async fills locally. This lets us validate the brain → exec → strategy → tracker path without touching a venue.

3. **Safe default startup**
   `exec` now starts in dry-run mode unless you explicitly request live mode.

4. **Live rollout gates**
   Live execution requires:
   - `--live-mode`
   - `--arm-live`
   - `--enable-live-venue <same-as---venue>`
   - `--max-order-notional > 0`
   - `target_qty * max_order_notional <= live_order_notional_cap`

### 14b. What the live gates mean

These guards exist to stop accidental order placement during rollout:

- `--live-mode`
  This is the switch that disables the default dry-run behavior.

- `--arm-live`
  This is an explicit acknowledgement that you really intend to leave simulation mode.

- `--enable-live-venue`
  This must exactly match `--venue`. It prevents broad shared configs from accidentally enabling live trading on the wrong venue process.

- `--live-order-notional-cap`
  This is the tiny-notional clamp. Startup fails if:
  `target_qty * max_order_notional > live_order_notional_cap`

  The default cap is `25` USD. That gives us a hard, easy-to-understand rollout ceiling.

### 14c. Dry-run examples

Recommended first step for any venue validation:

```bash
./build/exec \
  --venue binance \
  --brain-host 127.0.0.1 \
  --brain-port 9001 \
  --dry-run \
  --target-qty 0.01 \
  --max-order-notional 100 \
  --position-limit 250 \
  --cooldown-ms 1000
```

What this does:
- receives real signals from brain
- applies `ExecEngine` guards
- logs orders locally
- produces synthetic fills on the exec strand
- exercises `DeadlineOrderTracker` and E1 accounting without exchange risk

### 14d. Live-mode example

Only use this after dry-run and replay checks look correct:

```bash
./build/exec \
  --venue binance \
  --brain-host 127.0.0.1 \
  --brain-port 9001 \
  --live-mode \
  --arm-live \
  --enable-live-venue binance \
  --target-qty 0.10 \
  --max-order-notional 100 \
  --live-order-notional-cap 25 \
  --position-limit 250 \
  --cooldown-ms 1000
```

If any live rollout guard is missing, `exec` refuses to start.

### 14e. Recommended rollout order

Use this order every time we bring up a new venue adapter:

1. **Unit tests**
   Confirm parser, signing, and pure logic behavior locally.

2. **Replay / backtest**
   Feed recorded events through brain and validate that signal generation looks sane.

3. **Dry-run exec**
   Connect real brain signals into `exec --dry-run` and inspect logs, cooldown behavior, tracker behavior, and E1 accumulation.

4. **Venue testnet / sandbox**
   If the venue offers a safe environment, validate request/response plumbing there before any live orders.

5. **Live connectivity with trading still disabled**
   Confirm TLS, auth, subscriptions, and status handling without allowing order flow.

6. **Tiny live rollout**
   Start with one venue, one process, tiny notional, explicit live gates, and close observation.

### 14f. Operator checklist

Before live mode:
- confirm `brain` is running and signals look sane
- confirm `exec --dry-run` logs the expected venue side and price
- confirm cooldown and position-limit settings are deliberate
- confirm `target_qty * max_order_notional` is below the live cap
- confirm `--enable-live-venue` exactly matches `--venue`
- confirm only one venue process is being armed at a time

During first live rollout:
- tail exec logs continuously
- keep `position-limit` small
- keep `target-qty` tiny
- keep `max-order-notional` realistic and bounded
- stop immediately on unexpected fills, repeated timeouts, or wrong-side orders

**Minimal single-symbol (no persistence, no brain):**
```bash
./build/pop/pop --venue binance --base BTC --quote USDT --debug
```

**Multi-symbol on one venue:**
```bash
./build/pop/pop --venue binance --symbols BTC/USDT,ETH/USDT,SOL/USDT \
  --brain_ws_host 127.0.0.1 --brain_ws_port 8443 --brain_ws_path /
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
| `--symbols` | (none) | F3: Comma-separated symbol pairs, e.g. `BTC/USDT,ETH/USDT`. Overrides `--base`/`--quote`. |
| `--base` | (none) | Base asset for single-symbol mode (e.g. `BTC`). Required if `--symbols` is absent. |
| `--quote` | (none) | Quote asset for single-symbol mode (e.g. `USDT`). Required if `--symbols` is absent. |
| `--depthLevel` | 400 | Local order book depth (Bybit caps to nearest of 1/50/200 automatically) |
| `--brain_ws_host` | (none) | Brain hostname — enables publishing |
| `--brain_ws_port` | (none) | Brain port |
| `--brain_ws_path` | (none) | Brain WS path (e.g. `/`) |
| `--brain_ws_insecure` | false | Skip TLS cert check — dev only, never use in production |
| `--brain_ws_certfile` | (none) | mTLS client cert PEM for PoP→brain connection |
| `--brain_ws_keyfile` | (none) | mTLS client key PEM for PoP→brain connection |
| `--brain2_ws_host` | (none) | F4: Standby brain hostname — enables fan-out to primary + standby simultaneously |
| `--brain2_ws_port` | 443 | F4: Standby brain port |
| `--brain2_ws_path` | / | F4: Standby brain WS path |
| `--brain2_ws_certfile` | (none) | F4: Standby brain mTLS client cert PEM |
| `--brain2_ws_keyfile` | (none) | F4: Standby brain mTLS client key PEM |
| `--persist_path` | (none) | JSONL output file (`.gz` = compressed) |
| `--persist_book_every_updates` | 0 | `book_state` checkpoint frequency (0 = off) |
| `--persist_book_top` | 50 | Levels per side in `book_state` |
| `--rest_timeout_ms` | 8000 | REST snapshot request timeout in ms |
| `--max_msg_rate` | 0 | Warn if msgs/sec > 2× this value at heartbeat (0 = off) |
| `--validate_every` | 0 | Periodic `OrderBook::validate()` every N updates; resync on failure (0 = off) |
| `--require_checksum` | false | Resync if checksum absent on a checksum-capable venue (OKX only currently active) |
| `--log_level` | info | Log verbosity: `debug` \| `info` \| `warn` \| `error` |
| `--log_path` | (none) | Write log to file in addition to stderr |
| `--health_port` | 0 | D5: Plain-HTTP health endpoint port (0 = disabled). `GET /health` returns JSON. |
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

**Health endpoint** (when `--health_port` / `--health-port` is set):
```bash
# Brain
curl -s localhost:8081/health | python3 -m json.tool

# PoP
curl -s localhost:8080/health | python3 -m json.tool
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

---

## 11. Health Endpoint

Both brain and PoP expose a plain-HTTP health endpoint when started with `--health-port` / `--health_port`. Bind to `127.0.0.1` (localhost) — no TLS needed for local monitoring.

### Brain

```bash
./build/brain/brain --config config/brain.conf --health-port 8081
curl -s localhost:8081/health | python3 -m json.tool
```

```json
{
  "ok": true,
  "process": "brain",
  "uptime_s": 42,
  "synced": 3,
  "total": 5,
  "standby": false,
  "venues": [
    {"venue": "binance", "symbol": "BTCUSDT", "state": "synced",
     "feed_healthy": true, "age_ms": 150},
    {"venue": "okx",     "symbol": "BTCUSDT", "state": "synced",
     "feed_healthy": true, "age_ms": 80}
  ],
  "last_cross_s_ago": 12.4,
  "ws_clients": 2,
  "latency_us": {"p50": 920, "p95": 4200, "p99": 8800, "n": 1432}
}
```

- `ok: true` when `synced == total`; `ok: false` when `synced == 0`
- `age_ms` = milliseconds since last book update from that venue
- `last_cross_s_ago` = seconds since last arb cross was emitted (null = none yet)
- `ws_clients` = number of active PoP connections
- `standby: true` = brain is in passive standby mode (F4); promote with `kill -USR1 <pid>`
- `latency_us` = F5: detection latency percentiles in µs (null when no crosses yet)

### PoP

```bash
./build/pop/pop --config config/binance.conf --health_port 8080
curl -s localhost:8080/health | python3 -m json.tool
```

```json
{
  "ok": true,
  "process": "pop",
  "venue": "binance",
  "uptime_s": 38,
  "handlers": [
    {"base": "BTC", "quote": "USDT", "state": "SYNCED", "running": true, "resyncs": 0},
    {"base": "ETH", "quote": "USDT", "state": "SYNCED", "running": true, "resyncs": 1}
  ]
}
```

- `ok: true` when all handlers report `SYNCED`
- `resyncs` counts total resyncs since process start (never resets)

### Monitoring loop

```bash
# Watch health every 5 seconds
watch -n 5 'curl -s localhost:8081/health | python3 -m json.tool'
```

---

## 12. Production Deployment (systemd / supervisord)

The `deploy/` directory contains ready-to-use process supervisor configurations. See `deploy/README.md` for full setup instructions. A summary is below.

### systemd (Linux)

Install binaries and config files:
```bash
sudo cp build/brain/brain build/pop/pop /usr/local/bin/
sudo mkdir -p /etc/zidabot
sudo cp config/brain.conf /etc/zidabot/
sudo cp config/binance.conf /etc/zidabot/pop_binance.conf
sudo cp config/okx.conf    /etc/zidabot/pop_okx.conf
# ... repeat for bybit, bitget, kucoin
```

Install and start units:
```bash
sudo cp deploy/brain.service   /etc/systemd/system/
sudo cp deploy/pop@.service    /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now brain
sudo systemctl enable --now pop@binance pop@okx pop@bybit pop@bitget pop@kucoin
```

Check status:
```bash
sudo systemctl status brain
sudo journalctl -fu pop@binance
```

### supervisord (macOS / Docker)

```bash
# Install supervisor
pip install supervisor   # or: brew install supervisor

# Copy config
cp deploy/supervisord.conf /etc/supervisord.conf   # or ~/supervisord.conf

# Start
supervisord -c /etc/supervisord.conf
supervisorctl status
supervisorctl restart pops:*   # restart all PoP instances
```

### Signal handling

Both processes handle `SIGINT` and `SIGTERM` gracefully:
- Flush and close the output file (brain) / persist file (pop)
- Close WebSocket connections cleanly
- Exit with code 0

systemd sends `SIGTERM` on `systemctl stop`, waits up to `TimeoutStopSec=10s`, then sends `SIGKILL` if needed.

---

## 13. Backtesting with zidabot_replay

`zidabot_replay` is a standalone binary that replays historical JSONL files produced by
PoP's `--persist_path` through the **exact same** C++ arb detection engine that runs in
production (same `UnifiedBook`, same `ArbDetector`, same staleness guards). No TLS, no
WebSocket, no threads — pure stdin → stdout.

### Build

```bash
cmake --build build -j4 --target zidabot_replay
# Output: build/brain/zidabot_replay
```

### Basic usage

```bash
# Replay two venues, print every cross detected
zcat persist/pop-data/2026-03-05-20-27-39/binance.jsonl.gz \
     persist/pop-data/2026-03-05-20-27-39/okx.jsonl.gz \
  | ./build/brain/zidabot_replay

# Filter to crosses ≥ 0.5 bps
zcat binance.jsonl.gz okx.jsonl.gz \
  | ./build/brain/zidabot_replay --min-spread-bps 0.5
```

Each detected cross is written as one JSON line to stdout:

```json
{
  "sell_venue": "binance",
  "buy_venue": "okx",
  "sell_bid_tick": 5000500,
  "buy_ask_tick":  4999000,
  "spread_bps": 3.0,
  "ts_detected_ns":  1774474171337645757,
  "sell_ts_book_ns": 1774474171333554792,
  "buy_ts_book_ns":  1774474171333554792,
  "lag_ns": 4090965
}
```

- `sell_bid_tick` / `buy_ask_tick` are integer ticks (`price × 100`), no float rounding
- `lag_ns = ts_detected_ns − max(sell_ts_book_ns, buy_ts_book_ns)` — detection latency
- Log output (venue registration, sync events) goes to **stderr**; cross JSON goes to **stdout** — easy to separate

### Flags

| Flag | Default | Description |
|---|---|---|
| `--depth` | 50 | Order book depth per venue |
| `--min-spread-bps` | 0 | Suppress crosses below this threshold (bps) |
| `--max-spread-bps` | 0 | Suppress crosses above this threshold, 0 = no cap |
| `--rate-limit-ms` | 0 | Min ms between signals per (sell, buy) pair; 0 = unlimited (recommended for backtesting) |
| `--max-age-ms` | 5000 | Max individual book age for arb scan (ms) |
| `--max-price-deviation-pct` | 0 | Exclude venue if best_bid deviates > N% from median across synced venues; 0 = off |
| `--emit-books` | off | Emit a `{"type":"book"}` depth curve line after every event (see below) |
| `--book-depth` | 0 | Levels per side in book output; 0 = full configured depth |
| `--log-level` | warn | Log verbosity to stderr: `debug`\|`info`\|`warn`\|`error` |

> **Note:** `--rate-limit-ms` defaults to 0 (unlimited) unlike production brain (default 1000 ms). In backtesting you want every cross, not just the rate-limited subset.

### Input format

Input must be raw JSONL lines in the format written by PoP's `--persist_path` — the same three event types (`snapshot`, `incremental`, `book_state`) plus `status` events. All four are handled exactly as the live brain handles them.

**Source a single venue file:**
```bash
zcat binance.jsonl.gz | ./build/brain/zidabot_replay
```

**Interleave multiple venues (strict chronological order via merge-sort):**

For accurate cross-venue timestamps, events should be in global time order. The simplest approach is to merge-sort by `ts_recv_ns` before piping:

```bash
python3 - <<'EOF'
import gzip, heapq, sys

files = sys.argv[1:]
iters = [((json.loads(l)["ts_recv_ns"] if "ts_recv_ns" in (j:=json.loads(l)) else j.get("ts_book_ns",0)), l)
         for path in files
         for l in gzip.open(path, "rt")]
import json
streams = []
for path in files:
    def gen(p=path):
        with gzip.open(p, "rt") as f:
            for line in f:
                d = json.loads(line)
                yield d.get("ts_recv_ns", d.get("ts_book_ns", 0)), line.rstrip()
    streams.append(gen())
for _, line in heapq.merge(*streams, key=lambda x: x[0]):
    print(line)
EOF binance.jsonl.gz okx.jsonl.gz | ./build/brain/zidabot_replay
```

Or just concatenate if approximate ordering is acceptable (venues are independent books):
```bash
zcat binance.jsonl.gz okx.jsonl.gz | ./build/brain/zidabot_replay
```

### Python integration

`python/example_backtest.py` wraps the binary in a Python `replay()` function and outputs a pandas DataFrame:

```bash
python3 python/example_backtest.py \
  --binary ./build/brain/zidabot_replay \
  persist/pop-data/2026-03-05-20-27-39/binance.jsonl.gz \
  persist/pop-data/2026-03-05-20-27-39/okx.jsonl.gz \
  --min-spread-bps 0.5
```

In a notebook or script:

```python
import gzip, json, subprocess

def replay(files, binary="./build/brain/zidabot_replay", **kwargs):
    cmd = [binary]
    for k, v in kwargs.items():
        cmd += [f"--{k.replace('_','-')}", str(v)]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    crosses = []
    for path in files:
        with gzip.open(path, "rt") as f:
            for line in f:
                proc.stdin.write(line)
                proc.stdin.flush()
                out = proc.stdout.readline()
                if out.strip():
                    crosses.append(json.loads(out))
    proc.stdin.close()
    proc.wait()
    return crosses

crosses = replay(
    ["persist/pop-data/.../binance.jsonl.gz",
     "persist/pop-data/.../okx.jsonl.gz"],
    min_spread_bps=0.5,
)

import pandas as pd
df = pd.DataFrame(crosses)
print(df.groupby(["sell_venue", "buy_venue"])["spread_bps"].describe())
```

### Depth curves (`--emit-books`)

`--emit-books` adds a `{"type":"book"}` line to stdout after every event, showing the
full bid/ask level stack for the updated venue. Use this for feature engineering:
spread time-series, book imbalance, slippage estimates, and multi-level arb sizing.

```bash
zcat binance.jsonl.gz okx.jsonl.gz \
  | ./build/brain/zidabot_replay --emit-books --book-depth 10 2>/dev/null \
  > stream.jsonl
```

Each book line:
```json
{
  "type": "book",
  "venue": "binance",
  "ts_ns": 1774474890118230065,
  "bids": [[9520000, 50], [9519000, 120], ...],
  "asks": [[9521000, 80], ...]
}
```

- `bids` / `asks` are arrays of `[price_tick, qty_lot]` pairs
- `bids` sorted descending (best bid first); `asks` ascending (best ask first)
- `price_tick = price × 100` (integer, no float rounding)

Cross lines are emitted on the same stream with `"type":"cross"`. Filter in Python:

```python
import gzip, json, subprocess

proc = subprocess.Popen(
    ['./build/brain/zidabot_replay', '--emit-books', '--book-depth', '10'],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True
)

books, crosses = [], []
for path in ['binance.jsonl.gz', 'okx.jsonl.gz']:
    with gzip.open(path, 'rt') as f:
        for line in f:
            proc.stdin.write(line)
            proc.stdin.flush()
            out = proc.stdout.readline()
            if not out.strip():
                continue
            d = json.loads(out)
            if d['type'] == 'book':
                books.append(d)
            elif d['type'] == 'cross':
                crosses.append(d)
proc.stdin.close()
proc.wait()

import pandas as pd
# Spread time series (top-of-book)
book_df = pd.DataFrame([
    {'venue': b['venue'], 'ts_ns': b['ts_ns'],
     'bid': b['bids'][0][0] if b['bids'] else None,
     'ask': b['asks'][0][0] if b['asks'] else None}
    for b in books
])

# Book imbalance: (bid_qty_total - ask_qty_total) / (bid_qty_total + ask_qty_total)
def imbalance(b):
    bid_qty = sum(l[1] for l in b['bids'])
    ask_qty = sum(l[1] for l in b['asks'])
    total = bid_qty + ask_qty
    return (bid_qty - ask_qty) / total if total else 0.0

book_df['imbalance'] = [imbalance(b) for b in books]
```

### Save crosses to a file

```bash
zcat binance.jsonl.gz okx.jsonl.gz \
  | ./build/brain/zidabot_replay --min-spread-bps 0 2>/dev/null \
  > crosses.jsonl

wc -l crosses.jsonl                          # total crosses
jq '.spread_bps' crosses.jsonl | sort -n    # spread distribution
```
