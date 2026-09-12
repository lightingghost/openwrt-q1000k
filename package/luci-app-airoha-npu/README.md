# Airoha SoC Status for Q1000K

LuCI **Status → SoC Status** displays CPU frequency, temperature sensors,
NPU firmware/driver status and PPE entries, refreshing every five seconds.
Select `CONFIG_PACKAGE_luci-app-airoha-npu=y` with the LuCI feed installed.
The Q1000K community build profile selects this package.

Temperatures come from the existing AN7581 thermal driver and any registered
hwmon temperature devices. The page discovers sensors at runtime, converts
millidegrees to Celsius and reports missing sensors explicitly. No NCT7802
fan controller, fixed hwmon index or fan-control service is required.

CPU controls discover the CPU0 policy (or another registered policy directory)
and accept only frequencies/governors advertised by the kernel. Current
frequency uses `cpuinfo_cur_freq`, falling back to `scaling_cur_freq` when the
hardware readout is absent. The page refreshes controls if a policy appears
later, displays the kernel's actual readback after changes and reports errors.
Governor and maximum-frequency changes are runtime settings.

### Q1000K CPUFreq firmware compatibility

The reported boot failure was `cpufreq_policy_online: ->get() failed`, followed
by `cpufreq-dt: failed register driver: -19` and no policy directories. The
original clock provider assumes the vendor ATF SMC `0x82000301` / operation
`0xddddddd2` returns a CPU frequency in MHz. If it returns zero, the provider
reports zero Hz and CPUFreq rejects the policy before LuCI can use it. The log
establishes the missing clock rate; it does not contain the raw SMC reply.

The kernel now validates the firmware rate and retains SMC control when it
works. Q1000K explicitly opts into an AN7581 PLL fallback when SMC does not
supply a usable rate. This partially imports Ryan Chen's community fallback,
then adapts it to share the existing chip-SCU syscon and quiesce all CPUs during
clock transitions. It reads the PLL's fixed-point rate and post-divider,
checks clock-source/register readback, preserves unrelated register bits,
and keeps the backup clock enabled until the main clock is selected again.
Only the existing 500–1200 MHz OPPs are accepted; voltage settings and the
stock governor are unchanged. The LuCI apps never write raw PLL registers.

A kernel/firmware upgrade is required for this fix; installing only the LuCI
package cannot create a missing CPUFreq policy. After booting the updated
image, check:

```sh
dmesg | grep -Ei 'cpufreq|cpu frequency|direct PLL|ATF SMC'
ls /sys/devices/system/cpu/cpufreq
cat /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq
cat /sys/devices/system/cpu/cpufreq/policy*/scaling_available_frequencies
cat /sys/devices/system/cpu/cpufreq/policy*/scaling_available_governors
ubus call luci.airoha_npu getStatus
```

The kernel build and simulated register transitions are tested. Clock changes,
load stability and temperature still require validation on a physical Q1000K.
No device is flashed by these tests.

Raw GDM/CDM frame-engine register reads remain removed: the Ethernet driver
accumulates and resets those counters. FlowSense uses driver-provided Ethernet
statistics and a separate read-only PSE buffer snapshot.

The VLAN and PPPoE switches preserve the community RPC interface but are
labelled **bridge filtering**. They set `bridge-nf-filter-vlan-tagged` and
`bridge-nf-filter-pppoe-tagged`, respectively. They only affect bridge
netfilter when its IP hooks are enabled; the packaged defaults leave those
hooks disabled. They do not independently enable hardware offload or provide
standalone bridge acceleration. Use FlowSense's HW Flow Offload control for
the separate bridge flowtable service. Values persist in `/etc/sysctl.d/14-vlan-offload.conf`
and `/etc/sysctl.d/15-pppoe-offload.conf`; upgrade keep lists preserve them.

Q1000K uses the driver-default `airoha/en7581_npu_rv32.bin` unless DT supplies
a `firmware-name` override. A bound NPU driver indicates successful probing;
PPE bound entries, rather than that status alone, indicate offloaded flows.

## Origin

Created by [Ryan Chen (@rchen14b)](https://github.com/rchen14b), with VLAN/PPPoE
configuration by [Gilly (@Gilly1970)](https://github.com/Gilly1970).
Imported from the [W1700K applications pack](https://github.com/OpenWRT-fanboy/OpenW1700k/commit/c180c48b4e79ca9344a902bc3d9dcc4d4d57cbbd),
then adapted separately for Q1000K. Apache-2.0; see `LICENSE`.
