#! /usr/bin/env bash
# Builds ReBiosSandboxPkg against the EDK II tree given, or ./edk2, which
# must be at the tag in EDK2_TAG, its submodules included, and puts the
# sandbox firmware in ./output.
#
# Copyright (c) 2026, Mintsuki and contributors.<BR>
# SPDX-License-Identifier: BSD-2-Clause-Patent

set -e

here="$(cd "$(dirname "$0")" && pwd -P)"
edk2="$(cd "${1:-$here/edk2}" && pwd -P)"
target="${BUILD_TARGET:-RELEASE}"

if [ "$(git -C "$edk2" describe --tags --exact-match 2>/dev/null)" != "$(cat "$here/EDK2_TAG")" ]; then
    echo "error: '$edk2' is not at $(cat "$here/EDK2_TAG")" 1>&2
    exit 1
fi

export WORKSPACE="$here/Build-workspace"
export PACKAGES_PATH="$edk2:$here"
export EDK_TOOLS_PATH="$edk2/BaseTools"
mkdir -p "$WORKSPACE"

# edksetup.sh takes the positional parameters of whatever sources it as its
# own options.
set --
set +e
. "$edk2/edksetup.sh" >/dev/null
set -e
make -C "$edk2/BaseTools" -j"$(nproc)" >/dev/null

build -b "$target" -t GCC -a X64 -p ReBiosSandboxPkg/ReBiosSandboxPkg.dsc -n "$(nproc)"

mkdir -p "$here/output"
cp "$WORKSPACE/Build/ReBiosSandbox/${target}_GCC/FV/REBIOSSANDBOX.fd" "$here/output/rebios-sandbox.fd"
