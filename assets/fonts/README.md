# Fonts

The engine's **default font** is loaded from this directory (`assets/fonts/`).

Place a TrueType/OpenType file here and point the text stage at it, e.g.:

```
assets/fonts/default.ttf
```

## Status

Box painting (backgrounds, borders, solid rects) is implemented in the `Paint`
module and rasterised by `Paint::Canvas`. **Text painting is the next stage** and
will:

1. Load the default font from `assets/fonts/` (glyph rasterisation, e.g. via a
   bundled `stb_truetype`-style loader or FreeType).
2. Drive inline layout in the `Layout` engine (currently inline boxes fall back
   to block flow), producing positioned glyph runs.
3. Emit `CommandType::Glyph` paint commands consumed by `Canvas` (software) and,
   later, the Vulkan backend.

No font binary is committed yet — drop one in and wire the loader when the text
stage lands.
