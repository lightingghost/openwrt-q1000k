# Q1000K MCU firmware: destination, short files and OEM loading

Audited 2026-09-16 against the actual NAND `en7572.ko`, the native controller
loader, and Sirherobrine23 kernel commit
`2e2cf91fe84467d77649efebd99a28284f2124b3`. These are source/binary findings;
no OEM firmware was booted to make this comparison.

## Destination and inputs

All three loaders target the optical controller's **MD32 MCU**, with separate
16 KiB program memory (PM) and 4 KiB data memory (DM). PM and DM both begin at
memory offset zero; separate access ports select the memory. These offsets
are not Linux RAM or NAND addresses.

| Property | Sirherobrine23 EN7572 driver | Original Q1000K OEM | Native Q1000K loader |
|---|---|---|---|
| Input names | `airoha/en7572-pm.bin`, `airoha/en7572-dm.bin`; AN8901 has separate names | `/etc/lddla/A60993.elf.pm`, `/etc/lddla/A60993.elf.dm` | `/lib/firmware/airoha/q1000k/A60993.elf.pm`, `.dm` |
| Input API | Linux `request_firmware()` | `filp_open()` and `ecnt_kernel_fs_read()` | `request_firmware_direct()` plus exact length/SHA-256 checks |
| PM destination | PM offset 0, 16,384 bytes | Same | Same |
| DM destination | DM offset 0, 4,096 bytes | Same | Same |
| Calibration | Optional valid BOB, 512 bytes at DM `0x600` | `en7572_bob.conf`, 512 bytes at DM `0x600` | Required unit record; first 512 of the verified 513-byte DSD input at DM `0x600` |
| PM/DM config/address bank | A2 / 7-bit I²C `0x51` | A0 / `0x50` | A2 by default; explicit `oem-md32` experiment uses A0 |
| PM/DM data bank | A0 / `0x50` | A0 / `0x50` | A0 / `0x50` |
| MCU enable bank | A2 | A0 | A2 by default; A0 in `oem-md32` |
| Full memory readback before start | Not present in inspected loader | Not present in `Write_data_MD32()` | Mandatory, byte-for-byte over all PM and DM |

Register triplets (configuration, address, data port): PM
`0x3000 / 0x3004 / 0x3008`, DM `0x300c / 0x3010 / 0x3014`.
MCU enable is `0x3018`. Ordinary APD/OCP/reset/TX control remains on A2;
the OEM-A0 experiment changes only the MD32 bank selection.

The single bench already contains `oem-md32` and three
`oem-md32-acquire-<output>` combinations. Earlier acquisition captures tested
that bank hypothesis without recovering frames. Repeating those cases does
not require another image.

## Original OEM short-file behavior, verified from the NAND module

The original module's symbol table puts these buffers in `.bss` (zeroed when
the module is loaded):

- `flash_pm`: 16,384 bytes.
- `flash_dm`: 4,096 bytes.
- `flash_bob`: 512 bytes.

`Read_Data_From_Flash()` opens the fixed PM and DM paths. At offsets `0x678`
and `0x744` it requests respectively `0x4000` and `0x1000` bytes into those
buffers. It does not compare the returned read length with those capacities;
the load-success flags are set after successful file open, before the read.
Consequently, the normal initial load accommodates a shorter file and retains
the buffer's initial zeroes after the file's end. This is not a robust generic
short-read/error checker, and is not a promise about repeated debug reloads of
previously populated buffers.

`Write_data_MD32()` then writes 4,096 PM words and 1,024 DM words regardless of
the source file sizes. Its loop bounds are visible at `0x33c` and `0x418`.
It separately writes 128 BOB words starting at DM `0x600`. Thus a short *file*
does not mean a short *memory transfer*.

The actual NAND root filesystem contains:

| File | Bytes | SHA-256 |
|---|---:|---|
| `A60993.elf.pm` | 15,232 (`0x3b80`) | `5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1` |
| `A60993.elf.dm` | 56 (`0x38`) | `21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4` |

Both match the extracted update and current bench inputs. See the private
read-only extraction manifest at
`build-artifacts/q1000k-xgspon/nand-rx-audit-v3/reproduced/audit.json`.
The files' measured lengths are therefore expected for this firmware pair;
they are not an extraction truncation inferred merely from memory capacity.

## Native loader behavior

`package/kernel/q1000k-pon-control/src/driver.c:pon_initialize()` first checks
both known file lengths and hashes. Arbitrarily truncated, modified, or
externally padded replacements are rejected at this layer.

`src/en7573.c:en7573_load()` accepts nonempty PM up to 16 KiB and nonempty DM
up to calibration offset `0x600`; the upper bound prevents overlap with unit
calibration. The selected driver supplies the exact known 15,232/56-byte pair.

It then:

1. Identifies the selected XGS controller and asserts optical TX disable.
2. Holds the MCU, disables OCP/APD, waits 100 ms and performs the OEM core-reset
   sequence. It rechecks TX disable and the held MCU.
3. Streams **all 16 KiB PM** and **all 4 KiB DM**, word by word. `expected_word()`
   initializes each word to zero, copies available firmware bytes, and overlays
   the DM calibration region.
4. Reads every word back at an explicitly selected address and compares it with
   the expected firmware/zero/calibration word. Any mismatch stops startup.
5. Releases the MCU, waits 500 ms, and checks MCU-enable and optical TX-disable.

Expected native initial contents:

```text
PM 0x0000..0x3b7f  15,232 bytes of program file
PM 0x3b80..0x3fff   1,152 zero bytes

DM 0x0000..0x0037      56 bytes of data file
DM 0x0038..0x05ff   1,480 zero bytes
DM 0x0600..0x07ff     512 unit calibration bytes
DM 0x0800..0x0fff   2,048 zero bytes
```

Original OEM BOB processing also patches vendor-label bytes; the native path
preserves unit bytes and selects the known Q1000K PHY board profile separately.
That existing difference does not alter the PM/DM file-length explanation.

## Existing automated loader checks

`tests/q1000k/test_en7573.c:test_loader()` calls the production loader with
101-byte PM and 7-byte DM inputs, intentionally ending partway through a
four-byte word. It compares every PM/DM byte against firmware, zero padding or
the calibration overlay, then verifies that MCU startup occurs only afterward.
It runs for both A2-default and OEM-A0 addressing. It injects an I²C failure at
every operation and a memory-readback mismatch, checking that those failures
propagate and block startup. `test_pon_controller.py` includes this fixture in
the full bench host tests. These are fake-hardware tests; actual earlier bench
runs also required complete firmware readback before reporting initialization.

## Why the community driver rejects the original files

Its `en7572_request_image()` explicitly returns `-EINVAL` when
`fw->size < want`. `want` is the full 16 KiB or 4 KiB capacity. It copies exactly
that capacity into a `kmalloc()` buffer; it does not implement the OEM's
short-file/zero-tail convention. Longer files are accepted but only the first
capacity bytes are copied.

That is an input-format requirement of this implementation. It does not prove
that an MCU program must occupy every byte of PM, or that its initial data
must occupy every byte of DM. A program/data image can contain only populated
initial contents while runtime variables and unused space start at zero. This
is the natural explanation for the tiny DM image, but the exact MD32 linker
section map has not been recovered here.

**Conclusion:** short file length is not an outstanding missing-firmware
hypothesis for our loader. The actual OEM files match, the original startup
accommodates them, and our complete expanded transfers are verified. Readback
plus MCU-enable still does not prove correct execution, analog calibration,
high-speed electrical output or optical frame decoding. Those remain covered
by the controller/analog/clock experiments and pending hardware measurements.

## Reproduction and primary references

- NAND disassembly: `build-artifacts/q1000k-xgspon/nand-rx-audit-v3/pon-reference/en7572.ko.dis`,
  `Read_Data_From_Flash()` at `0x5c4`, `Write_data_MD32()` at `0x27c`,
  `init_module()` at `0xa90`. ELF symbols/sections establish `.bss` buffer sizes.
- [Pinned community memory loader and size checks](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/airoha/en7572_main.c#L194).
- [Pinned memory/register definitions](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/airoha/en7572_regs.h#L25).
- Native source: `package/kernel/q1000k-pon-control/src/driver.c`,
  `en7573.c`, `en7573.h`.
