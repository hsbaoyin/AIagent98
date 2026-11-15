# Emoji artwork

This folder holds the part of the AIagent98 Emoji Pack v2.0 that the GUI needs,
so a fresh clone has working emoji pictures without Git LFS, a download step or
network access at build time.

```
assets/emoji/
  bmp/24/A98Exxxx.bmp   1219 files, 24x24x24bpp, 1782 bytes each (~2.0 MiB)
  mapping/emoji.csv     source for src/gui/emoji_table.h
  LICENSE-NOTO.txt      SIL OFL 1.1 notice for the artwork
```

The picker (`a98_emoji_pick`) loads these BMPs into an image list with white as
the mask colour, so `src/gui/emoji.c` searches for `assets\emoji\bmp\24`
before any other layout.

Not tracked here: `bmp/16`, `bmp/32` and `png/72` from the full pack (~11 MiB).
Nothing in the code reads them. A release build can additionally ship
`emoji\bmp\24` next to the EXE, which the same search prefers when the
repository layout is absent.

`mapping/emoji.csv` regenerates the table:

```
powershell -File tools\gen_emoji_table.ps1
```

Artwork derives from Noto Color Emoji, licensed under the SIL Open Font License
1.1 — see `LICENSE-NOTO.txt`. Keep that file beside the BMPs when you
redistribute them.
