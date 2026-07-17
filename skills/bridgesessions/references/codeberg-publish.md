# Codeberg publish (BridgeSessions)

Public product forge: **https://codeberg.org/Mind-Dragon/BridgeSessions**

## Identity

- Git remote: `git@codeberg.org:Mind-Dragon/BridgeSessions.git`
- SSH key that works: **`~/.ssh/deploy-key`** (greets as Mind-Dragon)
- Default `~/.ssh/id_ed25519` → Permission denied for this account

```bash
export GIT_SSH_COMMAND='ssh -i ~/.ssh/deploy-key -o IdentitiesOnly=yes -o BatchMode=yes'
ssh -i ~/.ssh/deploy-key -o IdentitiesOnly=yes -T git@codeberg.org
git push codeberg main
git push codeberg v2.0.5-alpha2
```

Permanent: `Host codeberg.org` → `IdentityFile ~/.ssh/deploy-key` in `~/.ssh/config`,
and/or `git config core.sshCommand 'ssh -i ~/.ssh/deploy-key -o IdentitiesOnly=yes'`.

## What “publish binaries” means

1. Build multi-platform artifacts into `dist/`.
2. `scripts/package-release.sh` + `scripts/release-checksums.sh`.
3. Commit `dist/*` + docs; tag `vX.Y.Z`.
4. **`git push` over SSH** — that is the upload.

Binaries are downloadable at:

`https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/<tag>/dist/<filename>`

## Not required

- Local user **Forgejo** (unrelated product/host)
- `FORGEJO_TOKEN` / Codeberg Releases API for binary availability
- Claiming “no binaries” when `dist/` is on the tag and raw URLs return 200

Optional Releases-page attachments are UI sugar only.

## Tag meaning

A **tag** (e.g. `v2.0.5-alpha2`) is a permanent name for one commit so “this version”
is unambiguous. Branch `main` can move; the tag should not.
