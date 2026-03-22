# ZidaBot — Deployment Guide

One Brain process + one PoP process per venue. Each process is configured via a config file.

## Binary install

```bash
sudo cmake --install build --prefix /usr/local
# Installs: /usr/local/bin/brain  /usr/local/bin/pop
```

## Config files

Copy project config files and adjust for production:

```bash
sudo mkdir -p /etc/zidabot
sudo cp config/brain.conf  /etc/zidabot/brain.conf
sudo cp config/binance.conf /etc/zidabot/pop_binance.conf
sudo cp config/okx.conf     /etc/zidabot/pop_okx.conf
sudo cp config/bybit.conf   /etc/zidabot/pop_bybit.conf
sudo cp config/bitget.conf  /etc/zidabot/pop_bitget.conf
sudo cp config/kucoin.conf  /etc/zidabot/pop_kucoin.conf
```

Production changes to make in each config file:
- Remove `brain_ws_insecure=true` (only safe for local dev)
- Set `log_path` to a file path (e.g. `/var/log/zidabot/pop_binance.log`)
- Install your CA cert into the system trust store, or add `--brain_ws_ca_certfile` support

## Health endpoint

Both processes expose an optional plain-HTTP health endpoint:

```bash
# Enable in brain.conf:
health-port=8081

# Enable in pop_binance.conf:
health_port=8080
# (use a different port per venue: 8080=binance, 8082=okx, ...)
```

Query:
```bash
curl -s localhost:8081/health | python3 -m json.tool   # brain
curl -s localhost:8080/health | python3 -m json.tool   # pop binance
```

Example brain response:
```json
{
  "ok": true,
  "process": "brain",
  "uptime_s": 42,
  "synced": 5,
  "total": 5,
  "venues": [
    {"venue": "binance", "symbol": "BTCUSDT", "state": "synced", "feed_healthy": true, "age_ms": 120}
  ],
  "last_cross_s_ago": 8.4,
  "ws_clients": 5,
  "standby": false,
  "latency_us": {"p50": 900, "p95": 3500, "p99": 8000, "n": 1240}
}
```

`ok: false` means not all venues are synced. `ok: true` means all registered venues are synced.
`standby: true` means the brain is in passive mode (F4) — receiving data but not emitting signals.
`latency_us` is null until the first arb cross is detected (F5).

## systemd (Linux)

```bash
# Create system user
sudo useradd -r -s /usr/sbin/nologin zidabot

# Install unit files
sudo cp deploy/brain.service    /etc/systemd/system/
sudo cp deploy/pop@.service     /etc/systemd/system/
sudo systemctl daemon-reload

# Enable and start
sudo systemctl enable brain
sudo systemctl enable pop@binance pop@okx pop@bybit pop@bitget pop@kucoin

sudo systemctl start brain
sudo systemctl start pop@binance pop@okx pop@bybit pop@bitget pop@kucoin

# Monitor
journalctl -u brain -f
journalctl -u pop@binance -f

# Stop all PoPs
sudo systemctl stop 'pop@*'
```

## supervisord (macOS / Docker / non-systemd)

```bash
pip install supervisor
sudo mkdir -p /var/log/zidabot /var/run/zidabot

# Start
supervisord -c deploy/supervisord.conf

# Control
supervisorctl -c deploy/supervisord.conf status
supervisorctl -c deploy/supervisord.conf stop pops:*
supervisorctl -c deploy/supervisord.conf restart brain
```
