#!/usr/bin/env bash
# Package the built v2 Vulkan layer for the Adaptive Decky plugin.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LAYER_DIR="${LAYER_DIR:-$ROOT/build/lsfg-vk-layer}"
OUT_DIR="$ROOT/decky-plugin/bin"
ZIP_PATH="$OUT_DIR/lsfg-vk_noui.zip"

SO="$LAYER_DIR/liblsfg-vk-layer.so"
JSON="$LAYER_DIR/VkLayer_LSFGVK_frame_generation.json"

if [[ ! -f "$SO" || ! -f "$JSON" ]]; then
  echo "Layer artifacts not found in $LAYER_DIR" >&2
  echo "Build the layer first, e.g. cmake --build build --target lsfg-vk-layer" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp "$SO" "$STAGE/liblsfg-vk-layer.so"
cp "$JSON" "$STAGE/VkLayer_LSFGVK_frame_generation.json"

rm -f "$ZIP_PATH"
(cd "$STAGE" && zip -q -9 "$ZIP_PATH" liblsfg-vk-layer.so VkLayer_LSFGVK_frame_generation.json)

echo "Wrote $ZIP_PATH"
unzip -l "$ZIP_PATH"
