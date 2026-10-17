#!/bin/sh
# Apply our patches to pinned submodules (idempotent). Run from the repo root.
#   patches/<submodule>/*.patch  ->  lib/<submodule>
set -e
for dir in patches/*/; do
    sub=$(basename "$dir")
    for p in "$dir"*.patch; do
        if git -C "lib/$sub" apply --reverse --check "../../$p" 2>/dev/null; then
            echo "already applied: $p"
        else
            git -C "lib/$sub" apply "../../$p" && echo "applied: $p"
        fi
    done
done
