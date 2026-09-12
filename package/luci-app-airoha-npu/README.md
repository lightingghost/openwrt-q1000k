# Airoha SoC Status for Q1000K

LuCI **Status → SoC Status** displays CPU frequency, temperature sensors,
NPU firmware/driver status and PPE entries, refreshing every five seconds.
Select `CONFIG_PACKAGE_luci-app-airoha-npu=y` with the LuCI feed installed.
The Q1000K community build profile selects this package.

Temperatures come from the existing AN7581 thermal driver and any registered
hwmon temperature devices. The page discovers sensors at runtime, converts
millidegrees to Celsius and reports missing sensors explicitly. No NCT7802
fan controller, fixed hwmon index or fan-control service is required.

CPU controls accept only frequencies/governors advertised by the kernel.
They are runtime settings. Raw PLL overclocking and frame-engine register
reads were removed: polling read-clear counters can interfere with the
Ethernet driver's own statistics. CPU frequency is unavailable if the
stock firmware/driver does not expose a working cpufreq policy.

The VLAN and PPPoE switches preserve the community RPC interface but are
labelled **bridge filtering**. They set `bridge-nf-filter-vlan-tagged` and
`bridge-nf-filter-pppoe-tagged`, respectively. They only affect bridge
netfilter when its IP hooks are enabled; the packaged defaults leave those
hooks disabled. They do not independently enable hardware offload or provide
standalone bridge acceleration. Values persist in `/etc/sysctl.d/14-vlan-offload.conf`
and `/etc/sysctl.d/15-pppoe-offload.conf`; upgrade keep lists preserve them.

Q1000K uses the driver-default `airoha/en7581_npu_rv32.bin` unless DT supplies
a `firmware-name` override. A bound NPU driver indicates successful probing;
PPE bound entries, rather than that status alone, indicate offloaded flows.

## Origin

Created by [Ryan Chen (@rchen14b)](https://github.com/rchen14b), with VLAN/PPPoE
configuration by [Gilly (@Gilly1970)](https://github.com/Gilly1970).
Imported from the [W1700K applications pack](https://github.com/OpenWRT-fanboy/OpenW1700k/commit/c180c48b4e79ca9344a902bc3d9dcc4d4d57cbbd),
then adapted separately for Q1000K. Apache-2.0; see `LICENSE`.
