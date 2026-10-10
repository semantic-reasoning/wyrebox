#!/bin/sh
# Build and test WyreBox inside ci/ubuntu-26.04/Containerfile against both the
# installed Dovecot 2.4.2 headers and the repository fixture bundle.
set -eu

build_root=${WYREBOX_CI_BUILD_ROOT:-_build-ubuntu-26.04}

meson setup "$build_root/system" \
  -Dcapnp=enabled \
  -Dcapnp_serialization=enabled \
  -Ddovecot_backend=enabled \
  -Ddovecot_config=/usr/lib/dovecot/dovecot-config
meson compile -C "$build_root/system"
meson test -C "$build_root/system" --print-errorlogs

meson setup "$build_root/fixture" \
  -Dcapnp=enabled \
  -Dcapnp_serialization=enabled \
  -Ddovecot_backend=enabled \
  -Ddovecot_source_dir=tests/dovecot/fixtures/valid-2.4.2 \
  -Ddovecot_build_dir=tests/dovecot/fixtures/valid-2.4.2/build-config-valid
meson compile -C "$build_root/fixture"
meson test -C "$build_root/fixture" --print-errorlogs
