# Cross-OS net testing runbook

**Living document.** Edit in place as the binaries gain features. Per-session
results go in [`docs/cross-os-runbook-results/`](cross-os-runbook-results/)
(see the [template](cross-os-runbook-results/_template.md)) so each session
is bisectable from `git log docs/cross-os-runbook-results/`.

## Test matrix

Six interesting cells (3 server OSes × 3 client OSes, minus the
on-itself cases which are covered by spec-26 selftests):

|                  | client→ macOS | client→ Windows | client→ Linux |
|------------------|---------------|-----------------|---------------|
| **server↓ macOS**   | (skip)        | scenario A      | scenario B    |
| **server↓ Windows** | scenario C    | (skip)          | scenario D    |
| **server↓ Linux**   | scenario E    | scenario F      | (skip)        |

## Pre-flight (every session)

```bash
# 1. Fleet builds clean.
./scripts/fleet-build.sh
# expected: "fleet-build: N ok, 0 unreachable, 0 failed"

# 2. All three boxes reachable + recently built.
for os in macos windows linux; do
    bash -c "source scripts/fleet_lib.sh && fleet_run $os -- hostname"
done

# 3. Record the commit you're testing.
git rev-parse HEAD | tee docs/cross-os-runbook-results/$(date +%Y-%m-%d)-sha.txt
```

## Per-scenario flow

For each scenario:

1. On the **server box** (via `scripts/run-scenario.sh`):
   ```bash
   ./scripts/run-scenario.sh <server-os> --mission 1_Welcome --port 28000
   ```
   Wait for the banner. The banner prints the join commands for the
   other two boxes.

2. From a **client box** terminal (per the banner, paste exactly):
   - First: the `Probe from <os>` line — automated handshake check.
     Expect `phase-3: 1 ghost packets, 223 bytes total`.
   - Then: the `Join from <os>` line — opens the GL client window.

3. Run the per-scenario "Expected" checklist below.

4. **Server-side log** stays streaming in the run-scenario terminal —
   the per-client tick lines (`req=1 acc=1 data=1 ghost=1 unk=0`) prove
   the server saw the client.

5. `Ctrl-C` on the run-scenario terminal: clean shutdown propagates
   via SSH pty.

6. Drop a session result file under
   `docs/cross-os-runbook-results/YYYY-MM-DD-<scenario>.md`.

## Scenario A — server: macOS / client: Windows

```bash
./scripts/run-scenario.sh macos
```

**Expected:**
- Server log shows `[listener] bound on 0.0.0.0:28000`.
- Windows probe (paste line from banner) prints
  `phase-3: ≥1 ghost packets, ≥223 bytes total`.
- Windows client (`open-siege-t1-client.exe --server <mac-IP>:28000`)
  opens a GL window with the cycling background color.
- Server log shows `[listener] RequestConnect from <win-IP>:NNNNN ...`.

**Pass/fail:**
- [ ] Probe OK
- [ ] Client window opens
- [ ] Server logs the connection
- [ ] Clean disconnect on Ctrl-C

## Scenario B — server: macOS / client: Linux

```bash
./scripts/run-scenario.sh macos
```

Same as A but run the Linux probe + client. Linux client renders
into VNC/X11 (the Kubuntu VM has KDE so this works inside the VM
desktop).

## Scenario C — server: Windows / client: macOS

```bash
./scripts/run-scenario.sh windows
```

Validates the MinGW socket path bound + replies with the same template.
Run the macos probe + client.

## Scenario D — server: Windows / client: Linux

Same as C with Linux client.

## Scenario E — server: Linux / client: macOS

```bash
./scripts/run-scenario.sh linux
```

This is the **easiest** to drive since the user's primary box is macOS
and the Linux VM is always-on. Used as the smoke test for every commit
that touches net code.

## Scenario F — server: Linux / client: Windows

Same as E with Windows client.

## Known-good baseline (2026-05-23)

- Scenario E (Linux server / macOS net-test-client probe): PASS.
  `phase-3: 1 ghost packets, 223 bytes total in 3s`. Server log
  showed `req=1 acc=1 data=1 ghost=1 unk=0`.
- macOS↔macOS loopback (covered by spec 26 selftests in CI): PASS.

## What "shoot around" looks like (post-spec-26/13)

Once the client renders entities (spec 26/13+ work — currently the
client is a cycling-color SDL window only), the manual A/B becomes:

- Walk forward (W) — client smooth (no rubberband > 200ms)
- Look around (mouse) — server log shows movecommand stream
- Fire (LMB) — projectile visible, server reports hit/miss
- Quit (Esc) — clean shutdown, server log says `data` counter stops
  growing

Until then "shoot around" means "probe completes phase-3 + the client
window opens and shows the SDL placeholder background".

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `NO_REPLY` probe result | Firewall blocking UDP 28000 | Open UDP on server box |
| `HANDSHAKE_ONLY` | Server lacks ghost burst (pre-26/11 binary) | Rebuild via fleet-build.sh |
| `[listener] bind failed: Address already in use` | Orphan server | `pkill -f open-siege-t1-server` on that box |
| `Pseudo-terminal will not be allocated` | run-scenario invoked without TTY | Run interactively, not under `&` |
| Client GL window crashes on Wayland/headless Linux | No display | Run from KDE/GNOME session, or set `DISPLAY=:0` |
