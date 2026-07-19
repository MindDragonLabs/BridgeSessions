# TODO — P0 Security Blockers (Review 2026-07-16)

> Archived planning input from the 2.0.3/2.0.5 cycle. Current 2.0.6 closure
> status lives in `TODO-AUDIT-CLOSURE.md`.

**Status:** PARTIAL — core remediations landed in **v2.0.3** (2026-07-17)  
**Evidence:** unit tests `[security]` + MoA systems sweep `.audit/20260717-v203-security/AUDIT.md`

### Landed in v2.0.3
- [x] `verify_outbound_peer_identity` on mesh outbound (pin ↔ cert ↔ Hello) before `merge_peers`
- [x] `mesh.require_seed_pins` default **true**; skip dials without `pubkey=`
- [x] Inbound Hello pubkey must match client cert key
- [x] `sanitize_transfer_filename` + `path_is_inside_directory` + `transfer.max_bytes` (512MiB default)
- [x] TLS: min 1.2, max 1.3 (prefer 1.3; not 1.3-only — fleet RCA)
- [x] Version **2.0.3**; CMake project version aligned
- [x] Controller MoA systems checklist: OVERALL PASS

### Still open
- [ ] Attacker e2e (MITM / forged Hello) in CTest beyond unit helpers
- [ ] Full streaming hash (no large-file RAM) for transfers
- [ ] Docs: README/ARCHITECTURE still may claim TLS 1.3-only in places — reconcile
- [ ] Tag + fleet deploy of 2.0.3 to all nodes
- [ ] Modular tree archive/relabel as non-shipping

See `PHASES.md` for sequencing.
