# BridgeSessions marketing site

Next.js App Router source for the public marketing pages. GitHub is the only
primary repo and release channel:

https://github.com/MindDragonLabs/BridgeSessions

This directory is not a production domain cutover. Product documentation stays
in [`docs/`](../docs/) and https://minddragonlabs.github.io/BridgeSessions.

## Facts

Shipping facts are centralized in `lib/product.ts`. Keep them aligned with
`VERSION`, `SECURITY.md`, `LICENSE`, and the repo docs.

- Link the prerelease tag, not `/releases/latest`.
- Name the three shipping artifacts only (`linux-x86_64`, `macos-arm64`,
  `windows-x86_64.exe`).
- BSL 1.1 is source-available, not an Open Source license.
- Do not invent `uninstall.sh` / `uninstall.ps1`.
- Do not link production bridgesessions.com.

## Develop

```bash
npm install
npm run dev
```

```bash
npm run build
```
