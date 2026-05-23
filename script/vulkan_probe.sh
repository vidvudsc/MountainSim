#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD_DIR" --target VulkanProbe --parallel
"$BUILD_DIR/VulkanProbe"

