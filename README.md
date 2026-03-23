# zidaBot — Cross-Venue Arbitrage Detection Stack

A low-latency market data pipeline and arbitrage detector for crypto spot markets.

## Architecture

```
Exchange A (Binance) ──┐
Exchange B (OKX)     ──┤  TLS WebSocket (mTLS optional)
Exchange C (Bybit)   ──┤
Exchange D (Bitget)  ──┤                       ┌──────────────────────┐
Exchange E (KuCoin)  ──┘                       │  brain               │
                           PoP × N  ──────────►│  ArbDetector         │──► arb.jsonl
                                               │  UnifiedBook         │
                                               └──────────────────────┘
```

**PoP** (`pop/`) — one process per venue. Connects to the exchange WebSocket, maintains a live order book, and publishes normalized events to brain over a TLS WebSocket. Supports multiple symbols per process via `--symbols`.

**Brain** (`brain/`) — single central process. Accepts PoP connections, aggregates live order books, and scans every venue pair for cross-venue arbitrage crosses continuously.

## Quick Start

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt install cmake g++ libboost-all-dev libssl-dev zlib1g-dev
```

Requires: CMake ≥ 3.20, GCC ≥ 13 or Clang ≥ 17, Boost ≥ 1.73, OpenSSL, zlib.

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

Outputs: `build/brain/brain`, `build/pop/pop`

### Generate dev TLS certs

```bash
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout /tmp/brain_key.pem -out /tmp/brain_cert.pem -subj "/CN=localhost"
```

Or use the full mTLS setup: see [docs/HOWTO.md § 3](docs/HOWTO.md#3-tls--mtls-certificate-setup).

### Run (with config files)

```bash
# Terminal 1 — Brain
./build/brain/brain --config config/brain.conf

# Terminal 2–6 — one PoP per venue
./build/pop/pop --config config/binance.conf
./build/pop/pop --config config/okx.conf
./build/pop/pop --config config/bybit.conf
./build/pop/pop --config config/bitget.conf
./build/pop/pop --config config/kucoin.conf

# Watch arb crosses live
tail -f /tmp/arb.jsonl
```

Config files are in `config/` and reference certs in `/tmp/certs/`. Edit paths as needed.

### Multi-symbol PoP

```bash
./build/pop/pop --venue binance --symbols BTC/USDT,ETH/USDT,SOL/USDT \
  --brain_ws_host 127.0.0.1 --brain_ws_port 8443 --brain_ws_path /
```

### Health endpoints (optional)

```bash
./build/brain/brain --config config/brain.conf --health-port 8081
curl -s localhost:8081/health | python3 -m json.tool

./build/pop/pop --config config/binance.conf --health_port 8080
curl -s localhost:8080/health | python3 -m json.tool
```

## Documentation

| Document | Contents |
|---|---|
| [docs/HOWTO.md](docs/HOWTO.md) | Full end-to-end setup: build, TLS/mTLS, config, running, monitoring, health endpoint, production deployment |
| [docs/pop.md](docs/pop.md) | PoP architecture, all CLI flags, data flow |
| [docs/brain.md](docs/brain.md) | Brain architecture, all CLI flags, data flow |
| [docs/PERSISTENCE_SCHEMA.md](docs/PERSISTENCE_SCHEMA.md) | JSONL wire format for all event types |
| [deploy/README.md](deploy/README.md) | systemd and supervisord production setup |

## Supported Venues

| Venue | Sync Mode | Checksum |
|---|---|---|
| Binance | REST-anchored | None |
| OKX | REST-anchored | CRC-32 top-25 (active) |
| Bybit | REST-anchored | None |
| Bitget | REST-anchored | Disabled (seq gap) |
| KuCoin | WS-authoritative | None |

## References

- WebSocket RFC: https://datatracker.ietf.org/doc/html/rfc6455
- Boost.Beast: https://www.boost.org/doc/libs/1_89_0/libs/beast/doc/html/index.html
- Boost.Asio: https://www.boost.org/doc/libs/develop/doc/html/boost_asio/overview/rationale.html
