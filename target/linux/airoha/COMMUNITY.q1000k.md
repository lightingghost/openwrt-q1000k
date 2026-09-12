# W1700K community changes reviewed for Q1000K

Reviewed on 2026-09-12 against Q1000K base `3fcfdf845a` and the public
[W1700K releases](https://github.com/w1700k/builds/releases).
The fetched `OpenWRT-fanboy/OpenW1700k` `ubi2-oc` branch was
[`1b7cacf39aa15a0b8ce214d46305c0152e38cc6b`](https://github.com/OpenWRT-fanboy/OpenW1700k/commit/1b7cacf39aa15a0b8ce214d46305c0152e38cc6b).
The release page showed September 11 hashes; the branch had since been
rebased and gained a FlowSense follow-up. The table uses the fetched hashes.
Unrelated upstream commits between the two base revisions were not imported.

Q1000K shares the AN7581 SoC, PPE/NPU and Ethernet PCS with W1700K. It has
512 MiB RAM, one switch LAN port, an RTL8261N 10 GbE port, no Wi-Fi and no fan.
PON remains disabled. The user confirmed the absence of a fan and requested
temperature monitoring in its place.

## Commit decisions

All source hashes below refer to
[OpenWRT-fanboy/OpenW1700k](https://github.com/OpenWRT-fanboy/OpenW1700k).
Full imports preserve the source author and have `cherry picked from` trailers.
Partial imports preserve the author, identify the retained subset and include
`Source-commit` and `Source-repository` trailers. Q1000K adaptations follow
in a separate commit rather than being folded into the imported apps.

| Source commit | Subject / feature | Decision |
| --- | --- | --- |
| `e4f4dfb8b5` | Inject buildbot vermagic | Skip. A changed kernel must keep its own ABI hash; pretending to match snapshot modules is unsafe. |
| `d57aa14633` | Enable EIP93 crypto | Full cherry-pick. The SoC crypto device and driver already exist; include the module. |
| `4b786a7dc7` | CPUfreq / PM domain changes | Skip. Q1000K already has the `airoha,en7581` compatible and patch 607 fixes the positive attach-count check. This community patch additionally duplicates Kconfig symbols, replaces domain/OPP attachment and adds unvalidated direct PLL programming. |
| `01b95e9ebe` | DSA modules and raw netlink access | Skip. Ethernet/NPU/DSA are already built into Q1000K's kernel. The proposed NPU module forces MT7996 firmware; raw switch-register netlink access is unnecessary for the dashboards. |
| `947e986680` | Bridge-offload rule service | Skip with its kernel dependency below. Its dynamic nft rules alone cannot add bridge-family flowtable support. |
| `57c9119d3d` | Standalone L2 bridge offload | Defer. Review found an explicit `kfree_skb()` followed by `NF_DROP` in the fragmentation error path (netfilter core also frees on `NF_DROP`). Forward-path selection also treats any bridge-port ingress as bridging without limiting the new behavior to the bridge nft family. Requires correction and forwarding/encapsulation tests before import. |
| `a2910a20a4` | Reliability / compatibility bundle | Partial: apply 33 MHz SPI-NAND timing only to Q1000K. Skip W1700K LED/branding, regulatory, feed, audio and log-suppression changes. |
| `81e1668e2b` | HW1.1 / HW2.1 compatibility | Partial: retain E2 PCS manual RX calibration and PHY reset deassertion before MDIO enumeration. Keep the existing RTL8261N driver; the extra RTL8261CE driver and W1700K profile do not match this board. |
| `6b485caf7c` | Ramoops (PR #22473) | Full cherry-pick. Adds pstore and reserves 64 KiB at `0x86ff0000`, inside Q1000K RAM and outside existing ATF/NPU/QDMA reservations. Crash retention across resets still needs hardware verification. |
| `5ef246fcd7` | Default LRO plus QDMA synchronization | Partial: retain shared-QDMA GRO state synchronization and sibling traversal correction. Keep hardware GRO opt-in. |
| `1ec79ae1a7` | HSUART baud calculations | Full cherry-pick. Fixes shared AN7581/AN7583 UART divisor handling, including Q1000K's enabled secondary UART. |
| `4458a4566e` | Lorenzo patches / debug script | Skip. Changes concern Eagle Wi-Fi NPU SER, PCIe Wi-Fi initialization and a W1700K TX/debug script. |
| `f9b7af2b48` | MT76 source/firmware update | Skip: no Wi-Fi. |
| `cd95571291` | MT7996 debug counters / station stats | Skip: no Wi-Fi. |
| `97c5cc370a` | Truncated MT7996 TX-free handling | Skip: no Wi-Fi. |
| `97ea742858` | TX power controls | Skip: no Wi-Fi. |
| `81b0561f74` | wifi-scripts TX power (PR #23990) | Skip: no Wi-Fi. |
| `df93b53d96` | Beamforming / 6 GHz hostapd changes | Skip: no Wi-Fi. |
| `5244384f25` | Ethernet/NPU stability bundle | Defer Ethernet portion: combines new counters, IRQ locking changes, RX mapping and unverified meter-register retry logic; the retry code ignores read errors. The NPU-init changes affect WLAN memory setup and are not useful on Q1000K. |
| `c180c48b4e` | W1700K applications pack | Partial: import Airoha NPU and FlowSense, preserving their authorship notices. Skip fan control, Wi-Fi/MLO apps, speed tests and fastfetch. Add kernel sensor temperatures to SoC Status instead. |
| `2e85bf63bc` | FlowSense follow-up | Import the complete app commit. Its config-change event also works with stock firewall4; importing it does not imply importing standalone bridge offload. |
| `1b7cacf39a` | +200 MHz / performance governor | Skip: no Q1000K thermal or stability validation. Keep the stock OPPs and governor. |

## Q1000K app adaptations

- **Status → SoC Status**: thermal-zone and hwmon temperatures, CPU policy,
  NPU firmware/driver status and PPE entries. Sensors are discovered at
  runtime; absent sensors are reported explicitly. Fix the Q1000K thermal
  phandle to match the sensor's zero-cell binding.
- **Status → FlowSense**: PPE counts, existing Ethernet ports, CPU
  load, firewall flow-offload configuration and latency. Hide wireless gauges
  when there is no wireless PHY. Detect the plain AN7581 NPU firmware, with
  support for a DT firmware-name override.
- Keep original VLAN/PPPoE RPC methods, but label the controls accurately as
  bridge-netfilter filtering. They require enabled bridge IP hooks and do not
  independently enable hardware acceleration. Include `kmod-br-netfilter`,
  validate values and retain the sysctl configuration across sysupgrade.
  The package's default bridge IP hooks remain disabled.
- Remove direct PLL writes and raw frame-engine counter reads. The latter
  are managed and periodically reset by the Ethernet driver, so they are
  not reliable cumulative statistics for an independent polling app. CPU controls use advertised
  kernel policies only. Missing raw counters show unavailable rather than
  healthy. High RTT alone is not evidence of failed offload.
- Declare missing command dependencies, ensure executable RPC/init scripts,
  retain the latency config and use the standard LuCI installation hooks.
  Latency monitoring discovers the current IPv4 gateway; with no gateway,
  CPU sampling continues without sending probes to a hardcoded Internet host.

The separate `q1000k-build/user/q1000k/config.diff` enables both apps and
EIP93. Full source builds can select the same packages alongside `luci-ssl`.
They do not need the fan app, MT7996 firmware, overclocking, or a modified ABI
hash. No optical service or automatic hardware offload is enabled.

## Validation

See `tests/q1000k/README.md` for reproducible backend and page tests.
Hardware checks still required: temperatures against the serial/sysfs
readings, UART baud rates, cold-boot 10 GbE links on the actual silicon,
forwarding and GRO behavior, EIP93 use, ramoops retention and a normal
configuration-preserving sysupgrade. No device is flashed by this work.

## Branches

`main` tracks official OpenWrt upstream without Q1000K changes.
`q1000k-support` is reserved for explicitly requested upstream PR work.
All community imports and adaptations belong to `q1000k-dev`, which is
also the source branch selected by the separate firmware builder.
