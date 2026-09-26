# Normal Q1000K images with XGS-PON

This integration stays on `q1000k-xgspon`. The normal `quantum_q1000k-ubi`
target builds both:

- `*-quantum_q1000k-ubi-initramfs-recovery.itb`
- `*-quantum_q1000k-ubi-squashfs-sysupgrade.itb`

Both use `an7581-q1000k-xgspon.dts`, the ordinary UBI/NAND layout and the
same PON packages. Optical wiring is shared with the activation RAM image:
GPIO38 TX-disable gate, `ponraw` native DMA endpoint, enabled vendor PHY/MAC
and disabled overlapping native PCS. The normal DTS identifies itself with
`quantum,xgspon-service`. The separate bench profiles remain NAND-disabled
and RAM-only. `q1000k-dev`, the upstream support branch and the main builder
are unchanged.

## What was missing after the validated RAM checkpoint

Checkpoint `759ce36901fd2e919688a5e0edf05f5b3fd671ea` already contained the
Internet-tested controller, PHY, MAC, OMCI, encryption, service supervision
and bidirectional PPE implementation. The missing normal-image work was:

1. Selecting that runtime and its optical DT wiring in the UBI device profile.
2. Running the continuous recipe on normal boards without the private RAM
   marker or the validation-only controller parameter.
3. Installing `wan`/`wan6`, their firewall membership and IPv6 prefix layout.
4. Packaging shared OEM controller firmware while reading each unit's identity
   and calibration from its own UBI `factory` volume.
5. Selecting both image formats and verifying their actual packaged contents.

The supervisor retains the tested MAC options (including existing `bench_*`
parameter names), gates WAN startup on stable registration/authentication/OMCI
provisioning, and renews DHCP after signal recovery. Reverse ownership teardown
and fault containment remain in place. The OEM 802.1X comparison option is
restricted to private activation benches; normal service uses native OMCI.

## Build the pair

In a source checkout of `q1000k-xgspon`, with feeds and prerequisites ready:

```sh
cp target/linux/airoha/q1000k-xgspon.config .config
make defconfig
make -j8
```

The profile selects both formats, LuCI and PON without `CONFIG_BROKEN`.
AN7583's separate vendor driver selection still requires that gate.
PON modules load through the supervisor, not module autoload.

The local wrapper uses an existing configured cache, records source state,
configuration and feed revisions, builds and inspects both images, then
restores the previous build configuration:

```sh
python3 scripts/q1000k/image-build.py \
  --output ../build-artifacts/q1000k-xgspon/normal-images-UNIQUE \
  --jobs 8
```

This wrapper refuses a pre-existing `files/` overlay and accepts no private
input options. Both generic images include the checked `A60993.elf.pm` and
`A60993.elf.dm` in `q1000k-pon-firmware`. These shared MD32 program/data blobs
come from the stock Q1000K `/etc/lddla/`; see the
[firmware provenance](../../../package/firmware/q1000k-pon-firmware/README.md).
They contain neither the separately loaded calibration record nor subscriber
configuration.

## Factory data and private RAM benches

Normal images read the UBI volume named `factory` on the `ubi` NAND partition.
The reader validates the board, requires one matching volume and uses read-only
access. It reads WAN MAC at `0x5000`, LAN MAC at `0x6000`, unit serial at
`0x8000`, PON serial at `0x9000`, and the 513-byte calibration at `0xb000`.
The LAN bridge receives the validated factory LAN MAC. No raw-NAND DSD fallback
or staged calibration fallback is used by normal service, and private bench
serial/MAC overrides are ignored on the normal DT. Missing factory data is
reported as unavailable; it is never replaced by another unit's data.

Generic images have empty subscriber/OMCI identity fields, no calibration
file, no AT&T profile and no private-autostart marker. The first-boot configuration migration enables registration on untouched
normal UBI/recovery defaults. Diagnostic RAM defaults remain monitor-only.
A retained configuration, including a disabled setting, is preserved. A registration ID and
ISP provisioning values are not inferred from the factory layout. Configure
those explicitly in `/etc/config/xgspon`; `/etc/init.d/xgspon` starts the
watcher at boot and starts registration once valid identity is committed.
For manual recovery, set `xgspon.service.enabled=1`, run `uci commit xgspon`,
then `/etc/init.d/xgspon enable` and `/etc/init.d/xgspon restart`. Inspect
`xgspon status` and `omci -i pon status`. Committing changed identity/service settings through LuCI or
`uci commit xgspon` restarts the optical stack after validation. Identity edits
alone do not enable registration while `service.enabled=0`.

The private RAM-bench patch is held outside this repository under workspace
`build-artifacts/q1000k-xgspon/private-ram-patch-20260924/`. Its README describes
applying it to an isolated activation-profile build. It restores the previously
validated private identity, calibration and continuous-bench startup. Never
apply it to a normal image build or commit its contents to the source repository.
Older captured images keep their recorded identities and IP addresses; they are
not the generic outputs produced by this target.

## Network defaults and upgrades

LAN and preinit/failsafe default to `192.168.0.1/24`. Both copper ports remain
LAN. Normal images run LAN DHCP; bench profiles keep their separate DHCP/RA
server policy. Logical interfaces `wan` and `wan6` use `pon`, with the existing
firewall WAN zone. `ponraw` is the supervisor-owned DMA endpoint.

First installation applies `/64` LAN prefix assignment and DHCPv6 prefix
delegation. Software and hardware flow offloading default to enabled if those
settings are absent. Completion markers preserve subsequent UCI changes.
An existing unowned `wan` or `wan6` section is reported instead of overwritten.

`/lib/upgrade/keep.d/q1000k-xgspon` retains the PON UCI configuration and shared
firmware paths during configuration-preserving upgrades. Calibration stays in
`factory`; the upgrade handler continues to replace the FIT/rootfs volumes
without replacing `factory`. Discarding settings with `sysupgrade -n` restores
generic configuration. A saved user configuration can contain private values,
but those values are not part of the distributed generic image.

The initramfs has a RAM root filesystem and normal NAND visibility; its
factory reader needs the existing UBI factory volume to be available. The
sysupgrade FIT contains SquashFS, matching board metadata and the existing
256 MiB limit. Building either image performs no device operation.

## Validation limits

Host fixtures cover factory selection, normal service startup, registration
gating, signal recovery, network ownership, teardown and first-boot defaults.
`tests/q1000k/check_pon_normal_images.py` checks both FITs, DT wiring, actual
root filesystems, module parity, shared firmware, absent private overlays,
LAN defaults and sysupgrade metadata.

These software checks do not prove NAND boot or ISP acceptance. Cold boot
from UBI, a real settings-preserving sysupgrade, factory-data availability and
fiber recovery still require hardware acceptance. The preceding RAM
checkpoint's unresolved checks also remain: admission enforcement against
already bound PPE flows and repeated DHCPv6 reliability.
