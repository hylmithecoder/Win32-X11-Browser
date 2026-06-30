#!/usr/bin/env bash
# Headless launch + screenshot driver for DesktopWebview.
#
# DesktopWebview is a native X11/Vulkan GUI browser engine. It has no built-in
# file server and no CLI screenshot mode, so this driver wires up the pieces a
# headless agent needs:
#   * a rooted Xvfb display (grabbable; the host's rootless Xwayland is not)
#   * a python http.server (the engine only speaks http/https, never file://)
#   * the app launched against that display + URL
#   * a screenshot via ImageMagick `import -window root`
#
# Vulkan presentation needs DRI3 which Xvfb lacks; the app prints
# "vulkan: No DRI3 support detected" and falls back to software presentation.
# That is expected and non-fatal — the page still renders and is captured.
#
# Usage:
#   driver.sh page <file.html> [out.png]   serve the file's dir, load it, shoot
#   driver.sh url  <URL>        [out.png]   load an arbitrary URL, shoot
#                                           (start your own server first if local)
#
# Env overrides: DISP (default :99), PORT (8000), WAIT secs before shot (4).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"        # .claude/skills/run-desktopwebview -> repo root
BIN="$REPO/build-linux/DesktopWebview"
DISP="${DISP:-:99}"
PORT="${PORT:-8000}"
WAIT="${WAIT:-4}"

mode="${1:-}"; arg="${2:-}"; out="${3:-$HERE/shot.png}"
[ -n "$mode" ] && [ -n "$arg" ] || { grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[ -x "$BIN" ] || { echo "ERROR: $BIN not found. Build first: nix-shell --run build-linux" >&2; exit 1; }

XVFB_PID=""; HTTPD_PID=""; APP_PID=""
cleanup() {
  [ -n "$APP_PID" ]   && kill "$APP_PID"   2>/dev/null || true
  [ -n "$HTTPD_PID" ] && kill "$HTTPD_PID" 2>/dev/null || true
  [ -n "$XVFB_PID" ]  && kill "$XVFB_PID"  2>/dev/null || true
}
trap cleanup EXIT

# Rooted Xvfb big enough to contain the 1024x720 window placed at (100,100).
if ! DISPLAY="$DISP" magick import -window root /dev/null 2>/dev/null; then
  Xvfb "$DISP" -screen 0 1280x900x24 >/dev/null 2>&1 &
  XVFB_PID=$!
  for _ in $(seq 1 20); do DISPLAY="$DISP" magick import -window root /dev/null 2>/dev/null && break; sleep 0.2; done
fi

case "$mode" in
  page)
    [ -f "$arg" ] || { echo "ERROR: no such file: $arg" >&2; exit 1; }
    docroot="$(cd "$(dirname "$arg")" && pwd)"
    ( cd "$docroot" && exec python3 -m http.server "$PORT" ) >/dev/null 2>&1 &
    HTTPD_PID=$!
    for _ in $(seq 1 20); do curl -sf "http://localhost:$PORT/" -o /dev/null && break; sleep 0.2; done
    url="http://localhost:$PORT/$(basename "$arg")" ;;
  url)
    url="$arg" ;;
  *) echo "ERROR: unknown mode '$mode' (want: page | url)" >&2; exit 2 ;;
esac

echo "Loading: $url"
DISPLAY="$DISP" "$BIN" "$url" >"$HERE/app.log" 2>&1 &
APP_PID=$!
sleep "$WAIT"
if ! kill -0 "$APP_PID" 2>/dev/null; then echo "ERROR: app exited early:" >&2; cat "$HERE/app.log" >&2; exit 1; fi

DISPLAY="$DISP" magick import -window root "$out"
echo "Screenshot: $out"
