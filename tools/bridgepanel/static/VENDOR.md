# Vendored front-end plugins

Same-origin copies so BridgePanel stays Tailscale-only (no CDN, no GA).

| File | Package | Version | License |
|---|---|---|---|
| toastui-editor-all.min.js | @toast-ui/editor | 3.2.2 | MIT |
| toastui-editor.min.css | @toast-ui/editor | 3.2.2 | MIT |
| toastui-editor-dark.min.css | @toast-ui/editor | 3.2.2 | MIT |
| filepond.min.js | filepond | 4.32.12 | MIT |
| filepond.min.css | filepond | 4.32.12 | MIT |
| filepond-plugin-file-validate-size.min.js | filepond-plugin-file-validate-size | 2.2.8 | MIT |
| codemirror-bundle.min.js | CodeMirror 6 (@codemirror/* + legacy-modes) | 6.x (bundled 2026-08-25) | MIT |

Sources: `uicdn.toast.com/editor/3.2.2/`, `cdn.jsdelivr.net/npm/filepond@4.32.12/`, `cdn.jsdelivr.net/npm/filepond-plugin-file-validate-size@2.2.8/`.

SHA-256:

```
f50e1b7c0fc4e5d9a1ccd0d8be78cb3a950ccb3bf676fbf1627810c76aeaedd8  toastui-editor-all.min.js
c70e24c68fefc205e8e504edc07fd6a5efd3044a623b4be7e3ac16cc8a736ed9  toastui-editor.min.css
ed442a29f63a60567231efb1d17293e2b1e0ba8bdf8bc5176f9b55e01ad22001  toastui-editor-dark.min.css
15b4da486dba7b7d93687ac3dfa2ce64c00f1a0b5138b425ca55137b0508f6de  filepond.min.js
5de23a498a59d08711ccba32f0288b35cdabf8ce0e3bb6c11af2ed7191742b7d  filepond.min.css
5da1dc796e0d208e76c46dba68ec3eefb246f32124f7918066cf21ed902fd71a  filepond-plugin-file-validate-size.min.js
```

`usageStatistics: false` is required — Toast UI Editor otherwise pings a telemetry endpoint.
