#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/aipad-renderer-model-test"

mkdir -p "$build_dir"
cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/boards/shields/aipad/src" \
  "$repo_root/boards/shields/aipad/src/screenkey_renderer_model.c" \
  "$repo_root/tests/renderer_model_test.c" \
  -o "$build_dir/renderer_model_test"
"$build_dir/renderer_model_test"

printf 'ScreenKey renderer model test passed.\n'

