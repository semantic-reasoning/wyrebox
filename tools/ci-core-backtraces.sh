#!/bin/bash
# Capture core dumps from CI test runs and print their backtraces.
#
#   ci-core-backtraces.sh enable DIR   write cores to DIR (needs sudo)
#   ci-core-backtraces.sh report DIR   print a gdb backtrace for each core
#
# enable cannot raise the core size limit of its caller, so the shell that
# runs the tests must also run `ulimit -c unlimited`. report always exits 0
# so it never masks the test failure that triggered it.

set -u

usage() {
  echo "usage: $0 enable|report DIR" >&2
  exit 2
}

[ $# -eq 2 ] || usage
dir=$2

case "$1" in
  enable)
    set -e
    mkdir -p "$dir"
    dir=$(CDPATH='' cd "$dir" && pwd -P)
    # %E is the executable path with '/' replaced by '!'.
    sudo sysctl -w "kernel.core_pattern=$dir/core.%E.%p"
    ;;
  report)
    shopt -s nullglob
    cores=("$dir"/core.*)
    if [ ${#cores[@]} -eq 0 ]; then
      echo "ci-core-backtraces: no core files in $dir"
      exit 0
    fi
    for core in "${cores[@]}"; do
      name=${core##*/}
      name=${name#core.}
      exe=${name%.*}
      exe=${exe//!//}
      echo "::group::backtrace for $core"
      if [ -f "$exe" ]; then
        gdb -batch -nx \
          -ex 'info sharedlibrary' \
          -ex 'thread apply all bt' \
          "$exe" "$core" || echo "ci-core-backtraces: gdb failed for $core"
      else
        echo "ci-core-backtraces: cannot find executable $exe for $core"
      fi
      echo "::endgroup::"
    done
    exit 0
    ;;
  *)
    usage
    ;;
esac
