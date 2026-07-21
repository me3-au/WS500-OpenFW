#!/usr/bin/env sh
# Fetch build dependencies (STM32Cube HAL + CMSIS) into Drivers/.
# Idempotent; used by CI and developers alike. See Drivers/README.md.
set -eu

CUBE_TAG=v1.11.6
REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK="$REPO_ROOT/.deps/STM32CubeF0"

if [ -d "$REPO_ROOT/Drivers/STM32F0xx_HAL_Driver/Src" ] && \
   [ -d "$REPO_ROOT/Drivers/CMSIS/Include" ]; then
    echo "Drivers/ already populated — nothing to do."
    exit 0
fi

mkdir -p "$REPO_ROOT/.deps"
if [ ! -d "$WORK/.git" ]; then
    git clone --depth 1 --branch "$CUBE_TAG" --filter=blob:none --sparse \
        https://github.com/STMicroelectronics/STM32CubeF0.git "$WORK"
fi
cd "$WORK"
# CMSIS core headers are in-tree; the F0 HAL and device headers are submodules.
git sparse-checkout set Drivers/CMSIS/Include Drivers/CMSIS/Device/ST --skip-checks
git submodule update --init --depth 1 \
    Drivers/STM32F0xx_HAL_Driver \
    Drivers/CMSIS/Device/ST/STM32F0xx

mkdir -p "$REPO_ROOT/Drivers/CMSIS/Device/ST"
cp -R "$WORK/Drivers/STM32F0xx_HAL_Driver"        "$REPO_ROOT/Drivers/"
cp -R "$WORK/Drivers/CMSIS/Include"               "$REPO_ROOT/Drivers/CMSIS/"
cp -R "$WORK/Drivers/CMSIS/Device/ST/STM32F0xx"   "$REPO_ROOT/Drivers/CMSIS/Device/ST/"

echo "Dependencies fetched (STM32CubeF0 $CUBE_TAG) into $REPO_ROOT/Drivers/"
