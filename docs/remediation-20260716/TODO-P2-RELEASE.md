# TODO — P2 Release Quality (Review 2026-07-16)

**Status:** OPEN  
**Depends on:** Can start anytime; must complete before calling a release “production-secure.”

---

## P2-1 — Release metadata drift

**Finding:** Binary/CMake 2.0.1 vs README/BridgePanel 2.0.0, changelog ends 2.0.0, ARCHITECTURE still references v1.4/v1.7 “Phase 0”, ports 19949 vs 9948 vs 9943, feature checklist unchecked.

### Tasks
- [ ] **P2-1a:** Inventory all version strings (`rg '2\.0\.|v1\.|19949|9943|9948'`)
- [ ] **P2-1b:** Single `VERSION` file or CMake project version → inject CLI, doctor, BridgePanel build tag, docs
- [ ] **P2-1c:** Changelog entry for **2.0.1** (and pending 2.0.2 PowerShell/oneshot notes when tagged)
- [ ] **P2-1d:** Canonical port table in ARCHITECTURE.md only; other docs link it
- [ ] **P2-1e:** Update or delete stale “Next: Phase 0” / empty feature checklist
- [ ] **P2-1f:** `scripts/check-release-metadata.sh` fails CI on drift

**Done when:** Metadata checker green; human can answer “what ships?” from one page.

---

## P2-2 — Release provenance / supply chain

**Finding:** Annotated but unsigned tag; `dist/` binaries committed without SHA-256 manifest, signatures, SBOM, attestations, or CI release workflow.

### Tasks
- [ ] **P2-2a:** Stop treating git-tracked `dist/` as sole distribution **or** generate `dist/SHA256SUMS` in release script
- [ ] **P2-2b:** `scripts/release.sh`: clean tree → build matrix → ctest → package → checksums → optional cosign/minisign
- [ ] **P2-2c:** Produce SBOM (e.g. syft/cyclonedx) per platform artifact
- [ ] **P2-2d:** Sign git tags (SSH or GPG); document verification steps in README
- [ ] **P2-2e:** Optional: SLSA / build attestation from CI
- [ ] **P2-2f:** Document exact tests that ran for each release in `RELEASE-NOTES-x.y.z.md`

**Done when:** Third party can verify checksum + signature of a binary against a tag without trusting the git blob alone.

---

## P2 exit criteria
- [ ] Metadata checker + provenance artifacts exist for latest tag
- [ ] README “Verify a release” section works on a clean machine
