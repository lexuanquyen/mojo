#!/bin/bash
# Copyright 2013 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This a simple script to make building/testing Mojo components easier (on
# Linux).

# TODO(vtl): Maybe make the test runner smart and not run unchanged test
# binaries.
# TODO(vtl) Maybe also provide a way to pass command-line arguments to the test
# binaries.

do_help() {
  cat << EOF
Usage: $(basename "$0") [command|option ...]

command should be one of:
  build - Build.
  test - Run unit tests (does not build).
  perftest - Run perf tests (does not build).
  pytest - Run Python unit tests (does not build).
  gn - Run gn for mojo (does not sync).
  sync - Sync using gclient (does not run gn).
  show-bash-alias - Outputs an appropriate bash alias for mojob. In bash do:
      \$ eval \`mojo/tools/mojob.sh show-bash-alias\`

option (which will only apply to commands which follow) should be one of:
  General options (specify before everything):
    --debug / --release - Debug (default) / Release build.
  gn options (specify before gn):
    --clang / --gcc - Use clang (default) / gcc.
    --goma / --no-goma - Use goma (if \$GOMA_DIR is set or \$HOME/goma exists;
        default) / don't use goma.

Note: It will abort on the first failure (if any).
EOF
}

get_gn_arg_value() {
  grep -m 1 "^[[:space:]]*\<$2\>" "$1/args.gn" | \
      sed -n 's/.* = "\?\([^"]*\)"\?$/\1/p'
}

do_build() {
  echo "Building in out/$1 ..."
  if [ "$(get_gn_arg_value "out/$1" use_goma)" = "true" ]; then
    # Use the configured goma directory.
    local goma_dir="$(get_gn_arg_value "out/$1" goma_dir)"
    echo "Ensuring goma (in ${goma_dir}) started ..."
    "${goma_dir}/goma_ctl.py" ensure_start

    ninja -j 1000 -l 100 -C "out/$1" mojo || exit 1
  else
    ninja -C "out/$1" mojo || exit 1
  fi
}

do_unittests() {
  echo "Running unit tests in out/$1 ..."
  ./testing/xvfb.py "out/$1" mojo/tools/test_runner.py \
      mojo/tools/data/unittests "out/$1" mojob_test_successes || exit 1
}

do_perftests() {
  echo "Running perf tests in out/$1 ..."
  "out/$1/mojo_public_system_perftests" || exit 1
}

do_pytests() {
  echo "Running python tests in out/$1 ..."
  python mojo/tools/run_mojo_python_tests.py || exit 1
  python mojo/tools/run_mojo_python_bindings_tests.py "--build-dir=out/$1" || \
      exit 1
}

do_gn() {
  local gn_args="$(make_gn_args $1)"
  echo "Running gn with --args=\"${gn_args}\" ..."
  gn gen --args="${gn_args}" "out/$1"
}

do_sync() {
  # Note: sync only (with hooks, but no gyp-ing).
  GYP_CHROMIUM_NO_ACTION=1 gclient sync || exit 1
}

# Valid values: Debug or Release.
BUILD_TYPE=Debug
# Valid values: clang or gcc.
COMPILER=clang
# Valid values: auto or disabled.
GOMA=auto
make_gn_args() {
  local args=()
  # TODO(vtl): It's a bit of a hack to infer the build type from the output
  # directory name, but it's what we have right now (since we support "debug and
  # release" mode).
  case "$1" in
    Debug)
      args+=("is_debug=true")
      ;;
    Release)
      args+=("is_debug=false")
      ;;
  esac
  case "$COMPILER" in
    clang)
      args+=("is_clang=true")
      ;;
    gcc)
      args+=("is_clang=false")
      ;;
  esac
  case "$GOMA" in
    auto)
      if [ -v GOMA_DIR ]; then
        args+=("use_goma=true" "goma_dir=\"${GOMA_DIR}\"")
      elif [ -d "${HOME}/goma" ]; then
        args+=("use_goma=true" "goma_dir=\"${HOME}/goma\"")
      else
        args+=("use_goma=false")
      fi
      ;;
    disabled)
      args+=("use_goma=false")
      ;;
  esac
  echo "${args[*]}"
}

# We're in src/mojo/tools. We want to get to src.
cd "$(realpath "$(dirname "$0")")/../.."

if [ $# -eq 0 ]; then
  do_help
  exit 0
fi

for arg in "$@"; do
  case "$arg" in
    # Commands -----------------------------------------------------------------
    help|--help)
      do_help
      exit 0
      ;;
    build)
      do_build "$BUILD_TYPE"
      ;;
    test)
      do_unittests "$BUILD_TYPE"
      ;;
    perftest)
      do_perftests "$BUILD_TYPE"
      ;;
    pytest)
      do_pytests "$BUILD_TYPE"
      ;;
    gn)
      do_gn "$BUILD_TYPE"
      ;;
    sync)
      do_sync
      ;;
    show-bash-alias)
      # You want to type something like:
      #   alias mojob=\
      #       '"$(pwd | sed '"'"'s/\(.*\/src\).*/\1/'"'"')/mojo/tools/mojob.sh"'
      # This is quoting hell, so we simply escape every non-alphanumeric
      # character.
      echo alias\ mojob\=\'\"\$\(pwd\ \|\ sed\ \'\"\'\"\'s\/\\\(\.\*\\\/src\\\)\
\.\*\/\\1\/\'\"\'\"\'\)\/mojo\/tools\/mojob\.sh\"\'
      ;;
    # Options ------------------------------------------------------------------
    --debug)
      BUILD_TYPE=Debug
      ;;
    --release)
      BUILD_TYPE=Release
      ;;
    --clang)
      COMPILER=clang
      ;;
    --gcc)
      COMPILER=gcc
      ;;
    --goma)
      GOMA=auto
      ;;
    --no-goma)
      GOMA=disabled
      ;;
    *)
      echo "Unknown command \"${arg}\". Try \"$(basename "$0") help\"."
      exit 1
      ;;
  esac
done

exit 0
