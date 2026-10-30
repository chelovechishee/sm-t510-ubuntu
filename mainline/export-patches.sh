#!/bin/bash
# Export our kernel commits (everything after the "Linux 7.3" base commit of the
# tree mainline/build-kernel.sh made) to mainline/patches in the published
# form: no commit hashes, no dates, numbered NN/total.
# Usage: mainline/export-patches.sh   (KERNEL_SRC=/path for another tree)
set -euo pipefail

M=$(cd "$(dirname "$0")" && pwd)
T=$(dirname "$M")
K=${KERNEL_SRC:-$T/src/linux-7.3}

BASE=$(git -C "$K" rev-list --max-parents=0 HEAD)
rm -f "$M"/patches/0*.patch
git -C "$K" format-patch -q --zero-commit --numbered -o "$M/patches" "$BASE..HEAD"
# the Date: header is the only one in the first lines of each patch
sed -i '1,4{/^Date: /d}' "$M"/patches/0*.patch
ls "$M"/patches/0*.patch | wc -l
