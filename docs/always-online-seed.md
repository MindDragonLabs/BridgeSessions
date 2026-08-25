# Always-online seed

This page tells you how to run one central BridgeSessions node. Other nodes join that node. The examples use documentation addresses only.

## Goal

You have one node that:

- stays powered,
- keeps a stable listen address,
- issues invite tokens,
- stays in the mesh directory.

Call this node the **seed**. Call every other node a **peer**.

## 1. Pick the machine

Choose a host that does not sleep when you close a laptop lid. A small Linux server is the usual choice. A desk Mac that you set to never sleep also works.

The seed must be reachable on TCP **19949** from the other nodes. Use a private network or a VPN. Do not expose 19949 to the public internet unless you accept that risk and you understand [SECURITY.md](../SECURITY.md).

## 2. Install the binary

On the seed:

```bash
curl -fsSL https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.sh | bash
bs --version
```

The version must match the release that the rest of the mesh will run. Mixed date stamps on the same beta line still talk to each other. Mixed major protocol lines may not.

## 3. Name the node and bind an address

Edit `~/.bridgesessions/config`:

```ini
node.name seed-a
node.listen 192.0.2.10:19949
mesh.require_seed_pins true
mesh.mdns_enabled false
receive_dir ~/.bridgesessions/received
```

Rules:

- `node.listen` must be an address that other nodes can reach.
- Do not use `127.0.0.1` as the only listen address if remote peers must connect.
- Do not use `0.0.0.0` unless you also have a host firewall that allows only your mesh.
- Keep `mesh.require_seed_pins` set to `true`.

After the first start, the node writes its public key. Pin that key as a seed line so the node trusts itself after a restart:

```ini
seed seed-a 192.0.2.10:19949 pubkey=<64-hex-ed25519-public-key>
```

Get the public key from `bs doctor` or from the generated key file in `~/.bridgesessions/`.

## 4. Run the daemon as a service

Do not leave the daemon attached to an interactive SSH session.

### Linux (systemd user)

```bash
loginctl enable-linger "$USER"
systemctl --user enable --now bridgesessions.service
systemctl --user is-active bridgesessions.service
ss -tlnp | grep 19949
```

`enable-linger` keeps the user daemon after logout.

### macOS (launchd)

The installer writes `com.bridgesessions.mesh`. Check it with:

```bash
launchctl print "gui/$(id -u)/com.bridgesessions.mesh"
```

Set the Mac to prevent sleep if this Mac is the seed.

### Windows (Scheduled Task)

The installer registers task `BridgeSessions`. The task must have no execution time limit. Confirm the process listens on 19949.

## 5. Invite the first peer

On the seed:

```bash
bs invite
```

The output has two values:

1. The seed address and port.
2. A single-use token.

The token expires. Use it at once.

On the new node:

```bash
bs join 192.0.2.10:19949 <token> --start
bs health seed-a
bs peers list
```

`--start` starts the new node's daemon. The seed signs a directory enrollment. Other peers learn the new key by gossip.

## 6. Daily checks

```bash
bs --version
bs peers list
bs health seed-a
bs fleet
```

A healthy seed shows:

- a listen socket on 19949,
- `healthy (data-plane ok)` from at least one other node,
- the expected version string.

## 7. Upgrade the seed without dropping the mesh

Use the installer or `bs upgrade`. The current installer pauses the unit with a runtime mask. It does not persist-disable the unit.

After the swap:

```bash
bs --version
systemctl --user is-active bridgesessions.service   # Linux
bs health <any-peer>
```

If inbound shells fail with connection refused, the listener is down. Re-enable and start the service. Do not leave the unit disabled.

## 8. What not to do

- Do not publish the invite token.
- Do not put private host names or VPN addresses in public docs or public issues.
- Do not disable seed pins to “make join easier.”
- Do not restart the daemon through the same `bs shell` that depends on that daemon.
- Do not run two daemons that share one identity on two networks.

## Next

- [Quickstart](QUICKSTART.md)
- [Configuration](configuration.md)
- [Bridge Panel](bridge-panel.md)
- [Security](https://github.com/MindDragonLabs/BridgeSessions/blob/main/SECURITY.md)
