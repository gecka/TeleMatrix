# Emoji sprite atlases

`emoji_1.webp` … `emoji_8.webp` are the emoji sprite sheets consumed by
`src/ui/emoji/`. They are copied verbatim from Telegram Desktop.

| | |
|---|---|
| Source | `telegramdesktop/tdesktop:Telegram/Resources/emoji/` |
| Commit | `bd702c452113f5bca52843154c809dc882ba0b81` (branch `dev`) |
| Layout | 72 px cells, 32 per row, 16 rows per page → 512 emoji/page |
| Geometry | pages 1-7 are 2304×1152; page 8 is 2304×360 (5 rows) |

## The version constraint that makes these usable

An atlas is just pixels — the emoji↔cell mapping lives entirely in the generated
`emoji.cpp`, which `codegen_emoji` derives from `lib/lib_ui/emoji.txt`. **The two must
come from the same upstream revision or every emoji renders as the wrong picture.**

That match was verified before vendoring: our `lib/lib_ui/emoji.txt`
(md5 `70c9e87cba04a68c33b66a77a871ffac`) is byte-identical to the one in lib_ui commit
`632ae6ac4e1750900bbb2f40241b2e60eea00cef`, which is what the tdesktop commit above pins
as its `Telegram/lib_ui` submodule.

If you ever update `lib/lib_ui/emoji.txt`, re-vendor these files from a tdesktop commit
pinning the matching lib_ui, and re-check that md5 correspondence.

## Decoding requires the Qt WebP plugin

These are WebP, which is **not** part of qtbase — it comes from the `qtimageformats`
add-on. Without it `QImage(path, "WEBP")` returns null and every emoji silently
disappears. `tests/tst_emoji_atlas.cpp` is the gate that catches this in CI.

## Regenerating instead of copying

`codegen_emoji --images <path>` can build these from scratch — from a `NotoColorEmoji.ttf`,
a Twemoji checkout (`assets/72x72`), a JoyPixels checkout (`png/unicode/512`), or the
system Apple Color Emoji font on macOS. See `lib/codegen/codegen/emoji/generator.cpp:199-208`.
Nothing in the build invokes that mode; it is a deliberate manual step.
