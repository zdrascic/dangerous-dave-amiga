# Changelog

All notable changes to the Amiga port of Dangerous Dave.

## [1.1] - 2026-11-19

### Added
- **AGA version.** On an A1200 or A4000 the game loads `dave.aga`:
  8-plane graphics in the original's exact VGA colors on one 256-entry
  palette. Nothing to configure — the same program uses `dave.ocs` on
  OCS/ECS, and on AGA too when `dave.aga` is missing.
- The loading screen names a running RTG system (`RTG P96` for
  Picasso96, `RTG CGX` for CyberGraphX). The game itself always shows on the native video port.

### Changed
- The loading screen stays up at least 2 seconds, so the machine line can
  be read on a fast machine.

### Fixed
- Sprites no longer leave trails on AGA: the background saves behind them
  are allocated at the screen's depth (8 planes) instead of a fixed 5.
- Attract-mode demos replay without invulnerability even when debug mode is on, so
  a recording plays out as recorded.

## [1.0]

First release, for OCS/ECS Amigas (Kickstart 1.3+, 512 KB chip RAM).

- All ten levels, Dave's original movement feel, the monsters and their
  flight paths, the jetpack, the gun and the four warp zones.
- The intermission corridor, the title screen with its three help pages,
  the original's five attract-mode demos, GAME OVER, the high score table
  (`DSCORES.DAV`, saved on quit) and the congratulations at the end.
- The original PC-speaker sound effects played on Paula.
- Same game speed on PAL (50 Hz) and NTSC (60 Hz).
