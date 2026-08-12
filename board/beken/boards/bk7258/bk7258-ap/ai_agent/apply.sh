#!/bin/sh
############################################################################
# board/beken/boards/bk7258/bk7258-ap/ai_agent/apply.sh
#
# Applies this board's ai_agent patches to packages/ai_agent in the openvela
# work tree.  Idempotent: a patch that already reverse-applies is skipped.
#
# These changes belong upstream, not here.  They live in this repository only
# so the board can be built and flashed before the pull request against
# open-vela/packages_ai_agent (branch dev-ai-contest-2026) is merged; see
# README.md in this directory for what each patch does and why.
#
# Usage (from anywhere):
#   sh board/beken/boards/bk7258/bk7258-ap/ai_agent/apply.sh [--revert]
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

set -e

here=$(cd "$(dirname "$0")" && pwd)

# The openvela work tree root is this repository's parent directory: the repo
# manifest checks this repository out as a subdirectory of it.  From here that
# is seven levels up (ai_agent, bk7258-ap, bk7258, boards, beken, board, repo).

root=$(cd "$here/../../../../../../.." && pwd)
target="$root/packages/ai_agent"

if [ ! -d "$target/.git" ]; then
  echo "apply.sh: $target is not a git checkout" >&2
  exit 1
fi

revert=no
if [ "$1" = "--revert" ]; then
  revert=yes
fi

for patch in "$here"/[0-9]*.patch; do
  [ -e "$patch" ] || continue
  name=$(basename "$patch")

  if [ "$revert" = yes ]; then
    if git -C "$target" apply --reverse --check "$patch" 2>/dev/null; then
      git -C "$target" apply --reverse "$patch"
      echo "apply.sh: reverted $name"
    else
      echo "apply.sh: $name not applied, nothing to revert"
    fi
    continue
  fi

  if git -C "$target" apply --reverse --check "$patch" 2>/dev/null; then
    echo "apply.sh: $name already applied"
  elif git -C "$target" apply --check "$patch" 2>/dev/null; then
    git -C "$target" apply "$patch"
    echo "apply.sh: applied $name"
  else
    echo "apply.sh: $name does not apply cleanly -- upstream moved?" >&2
    exit 1
  fi
done
