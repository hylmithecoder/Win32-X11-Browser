---
name: run-desktopwebview
description: Build, launch, screenshot, and test DesktopWebview — the from-scratch C++ X11/Vulkan browser engine. Use when asked to run, start, build, screenshot, or test the app, or to confirm an engine change (Layout, Css, Paint, Net, JsEngine, Video, Image, Svg) renders a page correctly.
---

# Run DesktopWebview

DesktopWebview is a native X11/Vulkan GUI browser engine in C++26. It takes a
start URL as `argv[1]` (default `http://localhost/`), fetches it over real
HTTP/HTTPS (no `file://`), parses + lays out + paints it, and presents via
Vulkan. There is no built-in file server and no screenshot flag, so the agent
path is a driver that wires up a grabbable display, a local HTTP server, the
app, and a screenshot.

**Paths below are relative to the repo root** (the directory holding
`CMakeLists.txt`). The driver lives at
`.claude/skills/run-desktopwebview/driver.sh`.

## Prerequisites

All toolchain + libs come from the Nix shell (`shell.nix`). The driver also
uses these host tools, already present in this environment:

- `Xvfb` (rooted X server — the host's rootless Xwayland cannot be grabbed)
- `magick` (ImageMagick with the `x` delegate, for `import -window root`)
- `python3` (serves local pages), `curl` (readiness check)

## Build

The documented path enters the Nix shell and runs the `build-linux` shell
function. From the repo root:

```bash
nix-shell --run "build-linux"
```

This configures `build-linux/` (CMake, Debug) and compiles `DesktopWebview`
plus all `Test*` binaries. The output binary is `build-linux/DesktopWebview`.
It links against Nix-store `.so`s via baked rpath, so it also runs outside the
Nix shell once built.

## Run (agent path) — launch + screenshot

`driver.sh` starts a rooted Xvfb on `:99`, serves your page, launches the app,
waits, and grabs the root window to a PNG. Two modes:

```bash
# Serve a local HTML file's directory and load it:
.claude/skills/run-desktopwebview/driver.sh page <file.html> <out.png>

# Or load any URL (start your own server first if it's local):
.claude/skills/run-desktopwebview/driver.sh url <http://host/path> <out.png>
```

A concrete, verified invocation is in [Make a quick test page](#make-a-quick-test-page) below.

Output: prints `Loading: <url>` and `Screenshot: <path>`; writes the PNG (omit
the 3rd arg to default to `shot.png` next to the driver). The app's stdout/
stderr goes to `.claude/skills/run-desktopwebview/app.log`. The driver tears
down the app, server, and Xvfb on exit.

Knobs via env: `DISP` (display, default `:99`), `PORT` (default `8000`),
`WAIT` (seconds before the shot, default `4` — raise for heavy pages).

A captured page looks like: an address-bar chrome strip at top showing the URL,
then the page content below it. The current engine renders page text faintly in
light gray on white — a near-blank-looking capture with thin gray text at the
top of the content area is the **expected** output, not a failure.

### Make a quick test page

```bash
mkdir -p /tmp/dwv && cat > /tmp/dwv/index.html <<'EOF'
<!DOCTYPE html><html><head><title>Hi</title></head>
<body><h1>Daftar Nilai</h1><table border="1">
<tr><th>NO</th><th>Nama</th></tr><tr><td>1</td><td>Hylmi</td></tr></table></body></html>
EOF
.claude/skills/run-desktopwebview/driver.sh page /tmp/dwv/index.html /tmp/dwv/shot.png
```

## Run (human path)

The Nix shell exposes `run-linux`, which runs `build-linux/DesktopWebview`
against your real `$DISPLAY` (default URL `http://localhost/`, needs a server
on port 80):

```bash
nix-shell --run "run-linux"
```

Useless headless without a display + window manager; it opens a window and
blocks. Use the driver instead for automation. Navigate at runtime by typing a
URL into the address bar and pressing Enter (arrow keys move the cursor; mouse
clicks follow links; scroll wheel / Up-Down scroll).

## Test

The engine's parse/layout/paint/net/JS logic is covered headlessly (the paint
tests render to PPM and assert pixels). Most PRs here touch these internals —
this is the fastest signal:

```bash
nix-shell --run "run-test-linux"
```

Runs every `Test*` binary in sequence; non-zero exit if any fails. Individual
prebuilt binaries also run standalone, e.g.:

```bash
./build-linux/TestLayout
./build-linux/TestPaint
```

## Gotchas

- **The host display is rootless Xwayland (`:1`).** `import -window root` and
  `ffmpeg -f x11grab` both fail there — there is no X root pixmap, and this
  build of ffmpeg has no `x11grab`. `grim` works but captures the *host*
  desktop, not the app's nested window. The driver sidesteps all of this by
  running its own rooted **Xvfb** on `:99`, which has a grabbable root window.
- **`vulkan: No DRI3 support detected - required for presentation`** is printed
  on Xvfb and is **non-fatal** — the app falls back to software presentation
  and still renders + paints. Do not treat it as an error.
- **`[OpenCL] No platform found`** is also harmless (the OpenCL compositor path
  is optional; CPU paint is used).
- **No `file://` support.** The engine only speaks HTTP/HTTPS, so local content
  must be served (the driver's `page` mode does this). `localhost` resolves via
  the system resolver fallback after the hardcoded 1.1.1.1 DNS path returns
  empty for it.
- **Default window is 1024x720 at offset (100,100).** The driver's Xvfb is
  1280x900 so the whole window fits in the grab; smaller screens clip it.

## Troubleshooting

- `ERROR: .../build-linux/DesktopWebview not found` → run the Build step.
- `ERROR: app exited early` → the driver dumps `app.log`. A "Failed to connect
  to host" there means the URL's server isn't up (in `url` mode, start it
  first; check the port).
- Screenshot is solid black → the app window didn't map in time; raise `WAIT`
  (e.g. `WAIT=8 .claude/skills/run-desktopwebview/driver.sh page ...`).
