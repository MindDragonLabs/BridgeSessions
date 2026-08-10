# Codeberg publish (BridgeSessions)

Public product forge: **https://github.com/MindDragonLabs/BridgeSessions**

## Identity

- Git remote: `git@github.com:MindDragonLabs/BridgeSessions.git`
- SSH deploy key: **`~/.ssh/<deploy-key>`**
- Default `~/.ssh/id_ed25519` → Permission denied for this account

```bash
export GIT_SSH_COMMAND='ssh -i ~/.ssh/<deploy-key> -o IdentitiesOnly=yes -o BatchMode=yes'
ssh -i ~/.ssh/<deploy-key> -o IdentitiesOnly=yes -T git@github.com
git push codeberg main
git push codeberg v2.0.6-alpha2
```

Permanent: `Host github.com` → `IdentityFile ~/.ssh/<deploy-key>` in `~/.ssh/config`,
and/or `git config core.sshCommand 'ssh -i ~/.ssh/<deploy-key> -o IdentitiesOnly=yes'`.

## What “publish binaries” means

1. Commit the reviewed source and docs.
2. Build all platform binaries from that source commit, commit only those three
   versioned `dist/` binaries, and create the exact signed `vX.Y.Z` tag.
3. Generate source archives and metadata from that clean tag with
   `scripts/package-release.sh --release` and `scripts/release-checksums.sh`.
   `SHA256SUMS`, the SBOM, and source archives stay release assets rather than
   tracked tag content, because Git archives embed the commit ID they describe.
4. Run `scripts/gh release create --dry-run --draft-only`; only after that
   passes, run it with `--draft-only` to create a draft, upload every asset, and
   verify the downloaded bytes. Publishing the draft is a separate approval.
5. Push the reviewed commit and signed tag over SSH only after explicit approval.

Binaries are downloadable from the Codeberg release attached to the exact tag.

## Not required

- Any other forge (unrelated product/host)
- Committing generated source archives, checksums, or SBOM metadata to the tag
- Replacing assets on an already-published release

## Tag meaning

A **tag** (e.g. `v2.0.6-alpha2`) is a permanent name for one commit so “this version”
is unambiguous. Branch `main` can move; the tag should not.
