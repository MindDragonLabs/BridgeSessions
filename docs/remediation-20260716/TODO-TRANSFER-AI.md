# TODO — Large transfer + AI progress (v2.0.4)

## Problem
`bs file recv --wait` / `file send --wait` used a **fixed 120s deadline** and loaded whole files into RAM for SHA-256. ~500MB ≈ 10k×48KB chunks → timeout. IPC client timeout was also 120s. Worse than scp for large agent payloads.

## Requirements
- [ ] Size-aware overall timeout (e.g. max(5min, size/0.5MB/s), cap 2h)
- [ ] Idle timeout 120s without chunk progress (not global wall)
- [ ] Stream read/write + incremental SHA-256 (no full-file `std::string`)
- [ ] Emit `PROGRESS ...` lines ~every 10s: chunks, bytes, pct, rate_mibs, eta_sec
- [ ] CLI prints progress during `--wait`; final line `OK`/`ERROR`
- [ ] `transfer.max_bytes` default ≥ 2GiB (or 0 unlimited with warn)
- [ ] Smoke: 500MB random file mesh transfer Linux↔Windows

## Progress line format (stable for AI parsers)
```
PROGRESS phase=recv file=foo.bin chunks=120/10486 bytes=5898240/512000000 pct=1.2 rate_mibs=11.4 eta_sec=44
```
