# Usage

## Mesh and shells

```bash
bs peers list
bs health <peer>
bs fleet
bs stats
bs shell <peer>
bs shell <peer> --name agent
bs shell <peer> --cmd 'hostname && uptime'
bs sessions <peer>
```

Named sessions survive disconnect. Use one stacked shell or `run-script` for dependent work; separate one-shots do not share state.

## Files

```bash
bs file send <peer> <local> --wait
bs file send <peer> <local> --dest <remote> --wait
bs file recv <peer> <remote> --to <local> --wait
bs telemetry
```

Transfers are resumable and SHA-256 verified. Peers serve only from `receive_dir` by default.

## Scripts

```bash
bs run-script <peer> ./task.sh
bs script add ./task.sh --name task
bs script push task --peer <peer>
bs script run task --peer <peer> -- 'arg with spaces'
```

Aliases are path-safe; cached scripts are content-addressed. Arguments are shell-quoted individually.

## Computer use

```bash
bs cua screen <peer>
bs cua capture <peer> -o screen.png
bs cua click <peer> --x 500 --y 300
bs cua type <peer> --text 'hello'
bs capture-video <peer> --duration 10 -o capture.mp4
```

## Bootstrap and upgrade

```bash
bs invite
bs join <seed-address>:19949 <token> --start
bs upgrade
bs upgrade --tag 2026.08.24-beta7
```

Upgrade downloads GitHub Release assets and requires checksum/version verification.

## Daemon

```bash
bridgesessions --daemon --config ~/.bridgesessions/config
```

Platform installers generate service definitions. Do not copy obsolete templates from old tags.
