# Changes to doomgeneric

This is [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric) at commit
`e257a09527062bfb986b6e5e0780839b6cbd5b71`, modified for terraDial's game
firmware (env `waveshare-knob-1_8-doom`). GPL-2.0 section 2(a) asks for
modified files to say so and when; each change below is also marked
`terraDial:` in the source.

Licence: GPL-2.0-or-later, as the files' own headers say. The game firmware
is offered under GPL-3.0 (see `GAME_NOTICES.md` at the repository root).

## Removed

The platform back-ends this port doesn't use: `doomgeneric_*.c` other than
`doomgeneric.c` (SDL, Allegro, X11, Windows, Emscripten, Linux VT, Soso),
`i_sdlmusic.c`, `i_sdlsound.c`, `i_allegromusic.c`, `i_allegrosound.c`, the
makefiles and the Visual Studio project.

## Modified (2026-10-31)

| File | Change |
|---|---|
| `w_wad.c` | A missing graphic falls back to `TDBLANK` instead of stopping the game. |
| `w_file.c` | A port hook, `DG_OpenWad`, so the WAD can be read from flash. |
| `m_misc.c` | A port hook, `DG_FileExists`, for the same. |
| `p_switch.c` | Switch textures the WAD doesn't have are skipped, not fatal. |
| `i_system.c` | The zone is allocated from PSRAM; default and minimum zone sizes 5 and 3 MiB (were 6 and 6). |
| `info.c`, `info.h` | `states[]` is `const`, so it lives in flash; the Blot's (`MT_SERGEANT`) health is 60 (was 150). |
| `g_game.c` | Nightmare no longer quickens `states[]` tics (they're const); past the last map the game returns to MAP01; no story screens between maps. |
| `statdump.c` | `MAX_CAPTURES` 1 (was 32), to save internal RAM. |
| `net_defs.h` | `BACKUPTICS` 32 (was 128), to save internal RAM. |
| `m_menu.c` | `screenblocks` defaults to 11 (no status bar). |
| `i_video.h` | `SCREENHEIGHT` follows `DOOMGENERIC_RESY`, for a taller frame. |
| `st_stuff.h` | The status bar's Y stays at its 200-row layout. |
| `d_main.c` | Full-screen tests compare against `SCREENHEIGHT`, not a literal 200. |
| `p_pspr.c` | The pistol slot fires three shots 4.5 degrees apart. |
| `p_user.c` | Walking forward presses use every 8 tics, so doors open as you walk into them. |
