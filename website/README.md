# BridgeSessions marketing site

Next.js App Router source for the public marketing pages. GitHub is the only
primary repo and release channel:

https://github.com/MindDragonLabs/BridgeSessions

This directory is not a production domain cutover. Product documentation stays
in [`docs/`](../docs/) and https://minddragonlabs.github.io/BridgeSessions.

## Facts

Shipping facts are centralized in `lib/product.ts`. Keep them aligned with
`VERSION`, `SECURITY.md`, `LICENSE`, and the repo docs. Do not invent
uninstall steps, audit claims, or extra forges.

## Develop

```bash
npm install
npm run dev
```

```bash
npm run build
```
