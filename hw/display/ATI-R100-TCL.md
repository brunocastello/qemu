# R100 hardware TCL (transform) in the emulated Radeon

Status: milestone T1 (position transform).  Implemented in `ati_3d.c`
(`tcl_*`, `decode_vertex`), `ati_glr.c` (vertex shader `tcl` uniform).

Mac OS 9's OpenGL renderer (OpenGLRendererATI) draws with the chip's TCL
unit: `SE_CNTL_STATUS` bit 8 (TCL bypass) is clear and the vertices are in
object space.  RAVE sets bit 8 and sends window-space vertices (the original
RetroBoot model; that path is unchanged).

Sources used (public, read-only): Mesa 8.0 `src/mesa/drivers/dri/radeon/`
(`radeon_state_init.c`, `radeon_state.c`, `radeon_tcl.c`, `radeon_ioctl.c`,
`radeon_maos_arrays.c`, `radeon_maos_vbtmp.h`, `radeon_state.h`) and Linux
4.0/4.20 `drivers/gpu/drm/radeon/` (`radeon_reg.h`, `radeon_drv.h`,
`radeon_state.c` legacy UMS `radeon_emit_vectors/scalars`, `r100.c`
`r100_packet3_load_vbpntr`).  Nothing was checked against ATI's Mac driver
except through the rig log lines quoted in PLAN.md.

## Registers

| Register | Offset | Meaning |
|---|---|---|
| `SE_TCL_VECTOR_INDX_REG` | 0x2200 | [7:0] start octword, [23:16] octword stride, [31:28] dword count (unused here) |
| `SE_TCL_VECTOR_DATA_REG` | 0x2204 | writes 4 dwords per octword, address += stride after each octword |
| `SE_TCL_SCALAR_INDX_REG` | 0x2208 | [8:0] start dword (Mesa adds 0x100 for the "scalars2" bank), [23:16] dword stride |
| `SE_TCL_SCALAR_DATA_REG` | 0x220c | one dword per write, address += stride |
| `SE_TCL_OUTPUT_VTX_FMT` | 0x2254 | which components the TCL unit outputs (same bit layout as `SE_VTX_FMT`, NORM0 bit 18, Z0 bit 31) |
| `SE_TCL_OUTPUT_VTX_SEL` | 0x2258 | COMPUTE_XYZW/DIFFUSE/SPECULAR bits 0..2, texture input select nibbles at 16/20/24/28 |
| `SE_TCL_MATRIX_SELECT_0` | 0x225c | 4-bit matrix slots: MODELVIEW_0..3 at bits 0/4/8/12, IT_MODELVIEW_0..3 at 16/20/24/28 |
| `SE_TCL_MATRIX_SELECT_1` | 0x2260 | MODELPROJECT_0..3 at 0/4/8/12, TEXMAT_0..3 at 16/20/24/28 |
| `SE_TCL_UCP_VERT_BLEND_CTL` | 0x2264 | UCP enables (bits 2..7), fog mode (9:8), range fog (10), two-side (11), vertex blend (16+), cull (28..30), FORCE_W_TO_ONE (31) |
| `SE_TCL_TEXTURE_PROC_CTL` | 0x2268 | TEXGEN_TEXMAT_n_ENABLE bits 0..3, TEXMAT_n_ENABLE bits 4..7, texgen input nibbles at 16/20/24/28 |
| `SE_TCL_LIGHT_MODEL_CTL` | 0x226c | bit 0 lighting enable, 1 light in model space, 2 local viewer, 3/4 normalize/rescale normals, 5 specular lights, ... sources at 16/18/20/22 |
| `SE_TCL_PER_LIGHT_CTL_0..3` | 0x2270..0x227c | per-light flags, 16 bits per light |
| `SE_TCL_STATE_FLUSH` | 0x2284 | written before vector loads by the DRM |
| `SE_VPORT_XSCALE ... ZOFFSET` | 0x1d98..0x1dac | viewport scale/offset (floats) |
| `SE_CNTL` | 0x1c4c | bit 24 `VPORT_XY_XFORM_ENABLE`, bit 25 `VPORT_Z_XFORM_ENABLE`, bit 27 pixel centre (OGL) |
| `SE_COORD_FMT` | 0x1c50 | bit 0/1 XY/Z pre-multiplied by 1/W0, bit 16 `W0_IS_NOT_1_OVER_W0`, ST pre-mult bits 17/19/21/23 |
| `SE_CNTL_STATUS` | 0x2140 | bit 8 `TCL_BYPASS`, [1:0] vertex-array byte swap |
| `SE_VF_CNTL` / packet word | 0x2084 | bit 9 `TCL_ENABLE` (per primitive), bit 6 colour order RGBA |

R100 has no `SE_VTE_CNTL` (that is R200, 0x20b0).  On R100 the viewport
transform is gated by `SE_CNTL` bits 24/25 and the divide by W is decided by
`SE_COORD_FMT`.

Writes reach these registers either by MMIO or by CP `PACKET0` (the DRM and
Mesa use `CP_PACKET0(INDX, 0)` followed by `CP_PACKET0_ONE(DATA, n - 1)`,
the "one register" form).  `ati_cp.c` already handles `ONE_REG_WR`
(`reg` is not incremented); both routes end in `ati_mm_write_reg()`, which
calls `ati_3d_reg_write()`.  That hook now covers 0x1c00..0x22ff and feeds the
vector/scalar memories (`tcl_reg_write()`).  The raw register file also keeps
the last value written to each port.  The memories are not part of migration.

## Vector state map (octword addresses)

From `radeon_reg.h` (`RADEON_VS_*_ADDR`) and `radeon_state_init.c`:

| Address | Contents |
|---|---|
| 0, 4, 8, ... 60 | `RADEON_VS_MATRIX_n_ADDR`: sixteen 4x4 matrix slots (4 octwords each) |
| 64 / 72 / 80 / 88 / 96 / 104 (+ light index) | light ambient / diffuse / specular / dir-pos / hw vspot / attenuation (stride 8) |
| 112 | `VS_MATRIX_EYE2CLIP` (projection, 4 octwords) |
| 116 | user clip planes |
| 122 | global ambient |
| 123 | fog parameters |
| 124 | eye vector |

Scalar state: light DCD 0, spot exponent 8, spot cutoff 16, specular
threshold 24, range cutoff 32 (light index added, stride 8), guard-band
adjusts 48..51, shininess 60.

Mesa's slot assignment (`radeon_state.h`): 0 `MODEL_PROJ`, 1 `MODEL`,
2 `MODEL_IT`, 3..5 `TEXMAT_0..2`.  `SE_TCL_MATRIX_SELECT_0` = `MODEL |
MODEL_IT << 16`, `_1` = `MODEL_PROJ | TEXMAT_n << (16 + 4n)`; i.e. the select
fields hold slot numbers and the matrix lives at octword `slot * 4`.

Matrix layout (`upload_matrix`): octword `i` holds row `i` of the matrix
(Mesa's column-major `m[]` is read as `src[i], src[i+4], src[i+8], src[i+12]`),
so `clip_i = dot(octword_i, (x, y, z, w))`.  `MODEL_IT` is the inverse copied
verbatim (so its octword `i` is column `i` of the inverse = row `i` of the
inverse transpose).  Texture matrices swap rows 2/3 and optionally columns
2/3 (`radeonUploadTexMatrix`) so that the third output is q.

## Input vertex format with TCL

The draw packets (`3D_DRAW_VBUF/INDX/IMMD`, `3D_RNDR_GEN_INDX_PRIM`) carry an
`SE_VTX_FMT` word as without TCL, but it now describes object-space inputs:
`XY`, `Z` (bit 31), `W0` (bit 0; Mesa sets it for 4-component positions),
`N0` (bit 18, normal x/y/z), colours (`FPCOLOR/FPALPHA/PKCOLOR`), specular
and fog, `ST0..3` / `Q0..3`.  Dword order inside one vertex: position
(x y z [w]), normal, diffuse, specular/fog, texcoords
(`radeon_maos_vbtmp.h`).  With `3D_LOAD_VBPNTR` Mesa places each component in
its own array (position, normal, colour, ... in that order); word `i`'s low
byte is the dword count and bits 14:8 the stride (0 = constant).  The decoder
gathers a vertex from the arrays when the counts add up to the format's size.
`SE_COORD_FMT` as Mesa sets it for TCL: `W0_IS_NOT_1_OVER_W0 |
TEX1_W_ROUTING_USE_Q1`, i.e. nothing is pre-divided and W is the true w.

## Pipeline emulated (T1)

1. `tcl_begin()` per draw: TCL is used when `SE_CNTL_STATUS` bit 8 is clear and
   the primitive word has `VF_CNTL.TCL_ENABLE` (bit 9); packet 0x25 (RAVE) is
   never transformed.  `ATI_R100_TCL=off` disables, `=force` ignores bit 9.
2. Matrix: slot `SE_TCL_MATRIX_SELECT_1[3:0]` (MODELPROJECT_0).  If that slot
   is all zero, `EYE2CLIP (112) * MODELVIEW (select_0[3:0])` is composed.
3. Per vertex (`decode_vertex`): `clip = M * (x, y, z, w)` (z = 0 if absent,
   w = 1 if absent), then `x/w, y/w, z/w` and `rhw = 1/w` (sign kept), then
   the existing viewport step (`SE_CNTL` bits 24/25 and `SE_VPORT_*`).
   Colours and texture coordinates are taken as sent.
4. The renderer's vertex shader (`tcl` uniform) rebuilds `gl_Position` from
   `rhw`: with TCL, `w = 1/rhw` may be negative and z is not clamped, so GL's
   homogeneous clipping handles geometry that crosses the near plane.  The
   bypass shader path is unchanged.

`RE_TOP_LEFT` / the window offset are not added to vertices, in the bypass
path or here: the 3D target is addressed by `RB3D_COLOROFFSET/PITCH` and
`RE_WIDTH_HEIGHT`, and the driver bakes the window position into
`SE_VPORT_XOFFSET/YOFFSET` (Mesa `radeonUpdateWindow`, with a 0.125 sub-pixel
bias).  Mesa sets a negative `YSCALE` and `YOFFSET = drawable height`.

## Not emulated yet (logged once with `LOG_UNIMP`)

* T2: texture matrices and texgen (`SE_TCL_TEXTURE_PROC_CTL`), texture input
  select (`SE_TCL_OUTPUT_VTX_SEL` nibbles); texcoords pass through.
* T3: lighting (`SE_TCL_LIGHT_MODEL_CTL` bit 0): the vertex colour is used as
  is.  Normals are parsed and skipped.
* T4: fog (`UCP_VERT_BLEND_CTL` fog mode), user clip planes, vertex blending,
  `FORCE_W_TO_ONE`, two-sided lighting, guard-band adjusts.

## Traces

`-trace ati_3d_tcl_*`: `ati_3d_tcl_mat` (the four matrix rows, scaled by
1000, when the matrix changes; at most 64 times), then for the next three
TCL draws `ati_3d_tcl_vtx` (first vertex: object and clip coordinates) and
`ati_3d_tcl_win` (its window coordinates and the viewport registers).
`ati_3d_draw` shows prim/state as before; the stats string has `tcl_draws`.

## Unconfirmed

* Whether ATI's Mac driver uses the same matrix slots as Mesa (the code reads
  `SELECT_1[3:0]`; the trace prints both select words).
* Vector index stride semantics (stride applied per octword, 0 treated as 1)
  and the dword-count field (ignored).
* Whether the Mac driver sets `VF_CNTL` bit 9 on its TCL primitives, and
  whether it loads one array per component or interleaves.
* The dword position of N0 relative to W0 when blend weights are present
  (blend weights are not parsed).
