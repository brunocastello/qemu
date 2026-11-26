# Emulated ATI Radeon R100 (`ati-vga,model=r100`): provenance

The R100 model (Radeon 7000, PCI 1002:5144) in `ati.c`, `ati_2d.c`,
`ati_3d.[ch]`, `ati_cp.c`, `ati_gl.[ch]`, `ati_glr.c`, `ati_int.h`,
`ati_regs.h` and `ati_dbg.c` is taken from:

- RetroBoot 2.0.1, `emulator/patches/retroboot-ati-r100.patch` at RetroBoot
  commit `432a43d4ba8502a7afeaa5dd25ecf959c23aed1c` (SHA-256
  `6276b79e4d9e4ec771c1767b53272096272abc782a94a7abaf94cf8b39b3e393`), as
  imported by amcchord/ClassicMac (`ati/`, commit
  `bcee0a7d400312d3f37fb82d5abf19452416f3bb`).
- ClassicMac's `ati/classicmac-ati-macos.patch` (CGL / OpenGL 4.1 Core host
  back end loaded with `dlopen()`), `ati/classicmac-radeon-idle.patch`,
  `ati/classicmac-texture-swap.patch` and `ati/retroboot-radeon-cursor.patch`
  (RetroBoot `88d074a55b00af7507e9412734ee83fd988876b3`, cursor hunks only).
- Not ported: `ati/classicmac-host-resize.patch` and `host_display.[ch]`
  (they need ClassicMac's own display-control mailbox and guest driver); the
  WGL/Windows back end is not part of these patches.

License: GPL-2.0-or-later, as QEMU (`ati*.c` by BALATON Zoltan) and the
upstream patches.  No ATI or Apple binary is included.

## Rebase notes (QEMU 11.0.2 -> 10.0 UTM)

The patches apply to QEMU 11.0.2's `hw/display/ati*`, which carry upstream
changes this tree did not have yet (2D engine rewrite, HOST_DATA, linear
aperture size, scissor registers, I2CDDC embedded in the device). Those
upstream versions of `ati.c`, `ati_2d.c`, `ati_int.h`, `ati_regs.h` and
`ati_dbg.c` were taken whole as the base, then the patches were applied, then:

- `hw/core/qdev-properties.h` -> `hw/qdev-properties.h`;
  `class_init(..., const void *)` -> `void *`; `(const InterfaceInfo[])` ->
  `(InterfaceInfo[])`.
- `ati_3d.c`: `system/physmem.h` does not exist here (and `exec/ram_addr.h`
  is target-specific); the texture dirty test uses
  `memory_region_snapshot_and_clear_dirty()` + `memory_region_snapshot_get_dirty()`
  on the VRAM region instead of `physical_memory_test_and_clear_dirty()`.
- `meson.build` and `trace-events` hunks applied by hand.

Run-time switches: `CLASSICMAC_ATI_3D=off` (or `RETROBOOT_ATI_3D=off`) turns the
3D engine off; QOM `x-accel-stats` and `x-gl-renderer` report status.
