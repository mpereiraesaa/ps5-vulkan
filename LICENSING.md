# Licensing and source provenance

Unless a file states otherwise, the original work in this repository is:

Copyright (C) 2026 Manuel Pereira

It is licensed under the GNU General Public License, version 3 or (at your
option) any later version (`GPL-3.0-or-later`). The complete license text is in
[`LICENSE`](LICENSE).

Contributions intentionally submitted to this repository are accepted under
the same `GPL-3.0-or-later` terms. A contributor must have the right to submit
their change and must preserve copyright, SPDX and provenance notices from any
adapted source. This policy does not relicense third-party works that carry
their own notices.

## PS5 OpenGL provenance

The project may adapt compatible GPU implementation work from
[`blackbearreloaded/ps5-opengl`](https://github.com/blackbearreloaded/ps5-opengl),
which is also licensed under `GPL-3.0-or-later`. The reference revision used by
the current audit is:

- repository: `https://github.com/blackbearreloaded/ps5-opengl`
- commit: `7f9bfabdddb187a11e4401058eba8c9e55194d0a`
- author/project lead: BlackBearReloaded

Current derived or adapted files are:

| ps5-vulkan file | ps5-opengl reference | Relationship |
| --- | --- | --- |
| `native/runtime_shader.c` | `src/platform/ps5_agc_package.c` | AGC compiler-metadata/package ABI adaptation |
| `src/color_detile.c` | `src/gallium/ps5/ps5_screen.c`, `ps5_tiled_color_offset` | General 1/2/4/8/16-byte `SW_64K_R_X` tiled-address equations and surface sizing |
| `src/vk_sampler.c` | `src/gallium/ps5/ps5_screen.c`, `ps5_texture_descriptor_wrap` and fixed-border handling | GFX10.3 repeat, mirrored-repeat, edge/border clamp and fixed border-color encodings |
| `src/texture_format.c` | `src/gallium/ps5/ps5_screen.c`, `ps5_texture_descriptor_format`, `ps5_texture_descriptor_swizzle` and `ps5_texture_format_size` | GFX10.3 sampled-image format words, component selectors and texel sizes; unvalidated rows remain disabled candidates |
| `native/runtime_graphics_compiler.c`, `src/graphics_formats.h` | `src/gallium/ps5/ps5_screen.c`, `ps5_integer_vertex_format`, `ps5_packed_vertex_format` and PSBC vertex-format adaptation | Vulkan-to-PSBC GFX1013 vertex numeric-category mapping, including hardware-gated 8/16/32-bit and packed rows; only rows covered by ps5-vulkan's own gates are advertised |

Every future direct adaptation must add an SPDX identifier and identify its
source file and pinned revision in the file header and this table. Ideas and
observations that lead to an independent implementation should still be cited
when the reference materially influenced the design.

## Dependencies and distributed artifacts

Mesa/ACO, Vulkan-Headers, PSBC and the other pinned source dependencies retain
their own compatible licenses and notices. Generated SDK distributions include
this GPL license. Third-party source checkouts and generated artifacts are not
committed to this repository.

`libps5vk.a` is linked statically by the current native application workflow.
Anyone distributing a linked executable must satisfy GPLv3 corresponding-source
requirements for the combined work. Link-time import facades describe console
system-library symbols; proprietary console modules themselves are neither
copied into nor distributed by this project.
