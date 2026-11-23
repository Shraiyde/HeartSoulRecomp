#!/bin/sh
# Builds the 32-bit ARM Linux test binary inside the hns-port Docker image.
# Usage: port/build.sh [extra make args]
# First run: docker build -t hns-port port/docker
set -e
cd "$(dirname "$0")/.."
# CI sets HNS_MAKE_JOBS=2. An uncapped nproc compile on a small runner
# is killed mid-way, and the log stops without a compiler error.
jobs="${HNS_MAKE_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
exec docker run --rm -v "$PWD":/src -w /src hns-port \
  make linux BUILD=hns IS64BIT=0 PREFIX=arm-linux-gnueabihf- \
  ARCH_CFLAGS="-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -marm -fno-pic" \
  ARCH_ASFLAGS="-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard" \
  -j"$jobs" "$@"
