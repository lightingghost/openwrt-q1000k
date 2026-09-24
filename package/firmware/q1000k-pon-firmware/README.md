# Q1000K optical controller firmware

These are the stock Q1000K optical controller's MD32 program and initial data.
The OEM EN7572/EN7573 driver reads them from `/etc/lddla/` in its rootfs.
The checked NAND extraction and vendor firmware update contain identical files;
the same pair was used by the validated XGS-PON RAM image.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| A60993.elf.pm | 15232 | 5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1 |
| A60993.elf.dm | 56 | 21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4 |

The native loader checks both lengths and SHA-256 hashes before programming the
controller. It loads unit calibration separately at data-memory offset 0x600;
the calibration record is not part of either blob. This package contains no
subscriber identity, serial/MAC override, calibration record or ISP profile.

See [the extraction and loader audit](../../../target/linux/airoha/XGSPON-MCU-LOADER-AUDIT.q1000k.md)
for the source paths, extraction evidence and full memory-transfer behavior.
