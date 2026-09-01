# u8g2 (vendored)

Monochrome display library. This dongle uses it to drive the OLED panel over I2C.

Upstream: https://github.com/olikraus/u8g2
Upstream commit: 9a93ba2e383afb02313c4805b321316aa9b0d2da

## What's here, and what isn't

Only `csrc/` is vendored — u8g2's C sources, the ones this project actually links. Nothing
else upstream (the Arduino wrapper, the sys/ demo projects, docs) is needed here.

The tree is **unmodified except** for deleting two files: `csrc/u8g2_fonts.c` and
`csrc/u8x8_fonts.c`. Those two are 40.4 MB of `csrc`'s 43 MB — upstream's entire font
catalogue, compiled in as C arrays. This project only needs three fonts (ASCII plus the
Cyrillic block), so carrying the other several hundred would be 40 MB of dead weight in this
repository for no benefit.

Being unmodified otherwise — no reformatting, no local fixes, nothing else deleted — is
deliberate: a future update to a newer upstream commit is a `cp -R` of `csrc` followed by
re-deleting the same two files, not a merge against local edits.

The three fonts this project actually uses (`u8g2_font_10x20_t_cyrillic`,
`u8g2_font_9x15_t_cyrillic`, `u8g2_font_6x12_t_cyrillic`) are generated separately, from
upstream's own BDFs and its own `bdfconv` converter, by `tools/gen_dongle_fonts.sh`. Its
output is `firmware/s3/main/fonts_cyrillic.c` — generated, never hand-edited. That script
reads the `Upstream commit:` line above to pin the same revision this component vendors, so
the generated fonts and the vendored library never drift apart.

## Why not the component manager

ESP-IDF's component registry does carry a u8g2 package, but it is `nixy4/u8g2` v0.1.4: zero
downloads, published 2026-03. That doesn't meet this project's bar for a dependency this
firmware is pinned to — an unproven single-digit-version repackaging of someone else's
library is a worse provenance story than vendoring the upstream source directly and recording
its commit here. Vendoring also sidesteps the font-catalogue weight problem above, which a
component-manager dependency would have pulled in whole.

## Updating

```bash
git clone --depth 1 https://github.com/olikraus/u8g2.git /tmp/u8g2-src
rm -rf firmware/s3/components/u8g2/csrc
cp -R /tmp/u8g2-src/csrc firmware/s3/components/u8g2/
rm firmware/s3/components/u8g2/csrc/u8g2_fonts.c firmware/s3/components/u8g2/csrc/u8x8_fonts.c
```

Then update the `Upstream commit:` line above to the new commit's full SHA, and re-run
`tools/gen_dongle_fonts.sh` so the generated fonts track the same revision.
