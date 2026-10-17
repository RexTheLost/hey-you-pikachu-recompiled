#!/bin/sh
# Apply our patches to pinned submodules (idempotent). Run from the repo root.
set -e
for p in patches/rt64/*.patch; do
    if git -C lib/rt64 apply --reverse --check "../../$p" 2>/dev/null; then
        echo "already applied: $p"
    else
        git -C lib/rt64 apply "../../$p" && echo "applied: $p"
    fi
done
