# DesktopWebview Engine - Recommendation Notes

## Project Overview

DesktopWebview is a custom lightweight web rendering engine built from scratch using modern C++. The project has grown beyond a simple WebView wrapper and now contains several independent systems:

- HTML parsing pipeline
- CSS processing
- Layout calculation
- Painting / rendering pipeline
- JavaScript execution layer
- Networking
- Image and media handling
- Platform window abstraction
- Custom debugging system

The current direction is closer to a browser engine research prototype rather than a normal desktop application.

---

# Recommended Architecture Improvements

## 1. Modularize Source Structure

Current components should eventually be grouped by responsibility:

```
src/
├── core/
│   ├── Application
│   ├── Debugger
│   └── Lifecycle
│
├── dom/
│   ├── Node
│   └── Document
│
├── css/
│   ├── Parser
│   ├── Cascade
│   └── ComputedStyle
│
├── layout/
│   ├── BlockLayout
│   ├── InlineLayout
│   └── FlexLayout
│
├── render/
│   ├── Paint
│   ├── Font
│   └── Image
│
├── js/
│
├── network/
│
└── platform/
    ├── Linux
    └── Windows
```

This will make future browser features easier to add.

---

# Rendering Pipeline Recommendation

The most important next improvement is strengthening the rendering pipeline:

```
HTML
 ↓
DOM Tree
 ↓
CSS Rules
 ↓
Computed Style
 ↓
Layout Tree
 ↓
Display List
 ↓
Paint Commands
 ↓
OpenGL Renderer
```

Avoid connecting CSS directly into drawing. The browser should calculate what an element is before deciding how to draw it.

---

# CSS Engine Improvements

Recommended features to prioritize:

1. CSS specificity
2. Style inheritance
3. Computed style object
4. Box model
   - margin
   - padding
   - border
   - width
   - height
5. Flexbox support

Flexbox support will dramatically improve compatibility with modern websites.

---

# Layout Engine Improvements

Recommended order:

1. Block layout
2. Inline text layout
3. Box sizing
4. Flex layout
5. Absolute positioning
6. Overflow handling

Modern websites depend heavily on these features.

---

# Debugging System

The existing Debugger system is a strong foundation.

Recommended expansion:

```
DEBUG_DOM()
DEBUG_STYLE()
DEBUG_LAYOUT()
DEBUG_PAINT()
```

Example output:

```
Element: div
Style: display:flex
Layout: x=20 y=30 width=400 height=100
Paint: DrawRect()
```

This allows rendering bugs to be traced through the whole pipeline.

---

# Build System Recommendation

Keep the current CMake + Nix workflow.

Recommended:

- keep headers under a clean public include namespace
- separate engine library from executable shell
- expose stable API headers

Example:

```
include/
└── desktopwebview/
    ├── core/
    ├── browser/
    └── renderer/
```

---

# Long Term Research Direction

Possible research topics:

- GPU accelerated web rendering
- lightweight embedded browser engine
- parallel layout computation
- custom compositor
- browser engine optimization

---
