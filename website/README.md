# BridgeSessions marketing site

Next.js App Router source for the public campaign pages. GitHub is the only
download and source:

https://github.com/MindDragonLabs/BridgeSessions

This directory is not a production domain cutover. Operator docs stay in
[`docs/`](../docs/) and https://minddragonlabs.github.io/BridgeSessions.

## Voice

Write like a campaign landing page a smart 24-year-old can get in ten seconds.
Short paragraphs. Concrete verbs. Show the workflow first. Ask for one action:
install.

Do not use aviation English. Do not invent jargon. Do not put TLS, C++,
Ed25519, or license words on the public pages. Version is a quiet badge:
`2026.08.24-beta7`.

## Facts

Shipping facts are centralized in `lib/product.ts`.

- Link the prerelease tag, not `/releases/latest`.
- Name the three shipping artifacts only (`linux-x86_64`, `macos-arm64`,
  `windows-x86_64.exe`).
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
