#!/usr/bin/env bash
# Build an installable Decky plugin zip (same layout as the official CLI output).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN="$ROOT/decky-plugin"
NAME="Decky LSFG-VK Adaptive"
SAFE_NAME="Decky-LSFG-VK-Adaptive"
OUT_DIR="$PLUGIN/out"
ZIP_PATH="$OUT_DIR/${NAME}.zip"
COMMIT_PATH="$PLUGIN/${SAFE_NAME}.zip"

if [[ ! -f "$PLUGIN/plugin.json" ]]; then
  echo "plugin.json not found in $PLUGIN" >&2
  exit 1
fi

if [[ ! -f "$PLUGIN/bin/lsfg-vk_noui.zip" ]]; then
  echo "Layer zip missing; run scripts/package-decky-layer.sh first" >&2
  exit 1
fi

if [[ ! -d "$PLUGIN/node_modules" ]]; then
  (cd "$PLUGIN" && pnpm install)
fi

(cd "$PLUGIN" && pnpm build)

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
DEST="$STAGE/$NAME"
mkdir -p "$DEST/dist" "$DEST/bin" "$DEST/py_modules" "$DEST/i18n"

cp "$PLUGIN/LICENSE" "$PLUGIN/main.py" "$PLUGIN/package.json" \
   "$PLUGIN/plugin.json" "$PLUGIN/README.md" "$PLUGIN/shared_config.py" \
   "$DEST/"

cp "$PLUGIN/dist/index.js" "$DEST/dist/"
if [[ -d "$PLUGIN/dist/assets" ]]; then
  cp -a "$PLUGIN/dist/assets" "$DEST/dist/"
fi

cp "$PLUGIN/bin/lsfg-vk_noui.zip" "$DEST/bin/"
cp -a "$PLUGIN/py_modules/lsfg_vk" "$DEST/py_modules/"
find "$DEST/py_modules" -type d -name '__pycache__' -exec rm -rf {} + 2>/dev/null || true
find "$DEST/py_modules" -type f -name '*.pyc' -delete 2>/dev/null || true

cp -a "$PLUGIN/defaults/i18n/." "$DEST/i18n/"

mkdir -p "$OUT_DIR"
rm -f "$ZIP_PATH" "$COMMIT_PATH"
(cd "$STAGE" && zip -q -r -9 "$ZIP_PATH" "$NAME")
cp "$ZIP_PATH" "$COMMIT_PATH"

if [[ -d /opt/cursor/artifacts ]]; then
  cp "$ZIP_PATH" "/opt/cursor/artifacts/${SAFE_NAME}.zip"
fi

echo "Wrote $ZIP_PATH"
echo "Copied $COMMIT_PATH"
unzip -l "$ZIP_PATH"
ls -lh "$ZIP_PATH" "$COMMIT_PATH"
