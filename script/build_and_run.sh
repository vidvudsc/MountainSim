#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
APP_NAME="Mountains"
MODE="${1:-run}"

cd "$ROOT_DIR"

export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json}"

pkill -x "$APP_NAME" 2>/dev/null || true

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD_DIR" --target "$APP_NAME" --parallel
APP_BINARY="$BUILD_DIR/$APP_NAME"

case "$MODE" in
  run)
    "$APP_BINARY"
    ;;
  --debug|debug)
    lldb -- "$APP_BINARY"
    ;;
  --logs|logs|--telemetry|telemetry)
    "$APP_BINARY" &
    /usr/bin/log stream --info --style compact --predicate "process == \"$APP_NAME\""
    ;;
  --verify|verify)
    "$APP_BINARY" &
    PID=$!
    sleep 2
    if ps -p "$PID" >/dev/null; then
      echo "$APP_NAME launched successfully with PID $PID"
      kill "$PID" 2>/dev/null || true
      wait "$PID" 2>/dev/null || true
    else
      echo "$APP_NAME exited during launch verification" >&2
      wait "$PID"
    fi
    ;;
  *)
    echo "usage: $0 [run|--debug|--logs|--telemetry|--verify]" >&2
    exit 2
    ;;
esac
