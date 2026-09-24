# Observing an existing PON service

`live-collect.py` attaches through SSH to an already running router and records
optical/OMCI status, supervisor process identity, PON interface counters,
addresses, routes and existing WAN lease state. It also works with the
controller-only diagnostic monitor and labels that mode explicitly.

It does not load/unload modules, start/stop a service, initialize optics, change
TX permission, set identity, renew a lease, install addresses/routes, or change
WAN/LAN configuration. No helper is installed on the router. There is no remote
cleanup: stopping the collector leaves the running stack alone.

From the OpenWrt source checkout, with the router's SSH key already trusted:

```sh
python3 scripts/q1000k/live-collect.py \
  --output ../build-artifacts/q1000k-xgspon/live-observation \
  --samples 12 --interval 5
```

For RAM boots that use a separate verified SSH known-hosts file, add
`--known-hosts /path/to/known_hosts`. The collector uses strict host-key checking
and does not replace a key in the user's normal known-hosts file.

The default collection sends no test traffic. To request bounded reachability
probes using the existing PON connection, add one or more `--probe-ip` options:

```sh
python3 scripts/q1000k/live-collect.py \
  --output ../build-artifacts/q1000k-xgspon/live-reachability \
  --probe-ip 1.1.1.1 --probe-ip 2606:4700:4700::1111
```

Each probe requires a loaded full stack and a route through `pon`, binds ping to
that interface and sends at most three requests with a ten-second deadline.
It cannot pass by using the management connection. Ping success proves only
reachability to that address; it does not prove HTTPS, LAN forwarding, DNS,
throughput or stable Internet service. Probes are skipped if the full stack or
route is unavailable. A failed probe does not restart or reconfigure anything.

Snapshots record their host time and device uptime; their individual reads are
not atomic. A detected change of boot, supervisor process, module presence or
controller presence stops the collection and preserves both observations.
LOS, registration and address/lease changes can be observed within the same
service lifetime. When no supervisor process is available, module-presence
checks cannot prove that modules were never replaced between samples.

Output is a new private evidence directory containing `collection.json`,
individual snapshots, the exact collector/remote read commands, and checksums.
Factory and subscriber identity sections from the optical status response are
discarded before saving. Network addresses and lease state remain in the
private capture. Unsupported optional interfaces or commands are recorded as
unavailable. Interruptions and SSH failures preserve partial captures.

Exit codes: `0` means collection completed; `1` means collection failed;
`3` means the observed lifecycle changed; `130` means interrupted. Read
`probes` separately for probe outcomes. A successful collection never asserts
that optical or Internet service works. `--dry-run` prints the plan offline.

The existing `bench-collect.py` and `activation-collect.py` retain their exclusive
startup/teardown contract. Reset, cold-start, alternate firmware/parameters,
isolated TX, and network-reconfiguration experiments must use that contract.
Their cleanup cannot be applied to a service-owned live stack.
