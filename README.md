# DesktopWebview

DesktopWebview is a high-performance, custom-built toy desktop browser engine written in C++26. It features native windowing via X11 (Linux) and Win32 (Windows) APIs, Vulkan graphics integration, an uncompressed video player, dynamic page scrolling, and a custom JavaScript interpretation engine built entirely from scratch.

## Features

- **Multiplatform Native Windowing & Graphics**: Direct X11 and Win32 event loops with a high-performance Vulkan-ready presentation pipeline.
- **Custom JavaScript Engine (`JsEngine`)**:
  - Full lexical analysis (Tokenizer) and statement parsing.
  - Scoped variable environments, assignments, arithmetic evaluation, string concatenation, and equality checks.
  - High-fidelity DOM integration: `console.log`, `document.title`, `document.getElementById`, and `element.innerText` rendering reflow integration.
- **Raw Video Presentation (`.rawv`)**: Uses a custom uncompressed video format for high-framerate rendering with precise timing synchronization and frame blitting.
- **Interactive URL Chrome & Navigation**:
  - Live cursor navigation (Left/Right arrow keys) and text editing.
  - Interactive hyperlink selection (mouse click detection via layout tree coordinate querying).
- **Smooth Page Scrolling**: Responsive scroll wheel and keyboard (Up/Down arrow key) support.

## Building and Running

### Linux Natively
This repository contains a Nix development shell setup (`shell.nix`) containing all necessary tooling and dependencies.

1. **Enter the Nix Shell**:
   ```bash
   nix-shell
   ```
2. **Build the Project**:
   ```bash
   build-linux
   ```
3. **Run the Browser**:
   ```bash
   run-linux
   ```
4. **Run Unit Tests**:
   ```bash
   run-test-linux
   ```

### Cross-Compiling for Windows
1. **Enter the Nix Shell** and configure cross-compilation:
   ```bash
   nix-shell --arg strSystem "x86_64-linux"
   ```
2. **Compile using the MinGW Toolchain**:
   ```bash
   build-windows
   ```

## Project Structure

* `include/` & `src/`: Core engine implementations (JsEngine, Network, Parser, CSS, Layout, Graphics presentation).
* `test/`: Native unit testing targets verifying JS interpretation, networking, and rendering.
* `assets/`: Default user-agent styles and preloaded page resources.

# License
MIT License
[MIT](LICENSE) — © 2026 Hylmi