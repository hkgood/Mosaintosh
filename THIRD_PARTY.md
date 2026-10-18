# Third-party code

Mosaintosh as a whole is distributed under GPL-3.0-or-later (see `LICENSE`).
It builds on:

| Component | Where | Licence |
|---|---|---|
| umac (Matt Evans), the Mac Plus emulator | `external/umac` | MIT |
| Basilisk II disc driver code (Christian Bauer), inside umac's `disc.c`, `b2_macos_util.h`, `macsrc/sonydrv.S` | `external/umac` | GPL-2.0-or-later |
| Musashi 68000 core (Karl Stenerud) | `external/umac/external/Musashi` | MIT |
| Mosaico board simulator, host builds only | `sim/board` | Apache-2.0 |

umac's `include/keymap.h` and `keymap_sdl.h` come from Mini vMac (GPL-2.0)
and are used only by umac's SDL front end (`unix_main.c`), which Mosaintosh
does not build.

## Not included, never to be committed

The Macintosh Plus ROM and Mac OS system software are Apple's. They are not
part of this repository and must not be added to it or redistributed. Use
your own copies, locally, in `assets/` (ignored by git).
