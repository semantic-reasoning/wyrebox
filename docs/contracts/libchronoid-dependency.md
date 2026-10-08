# libchronoid Dependency Contract

## Status

Accepted for issue #337.

## Scope

This contract defines how WyreBox acquires and links libchronoid, and what
redistributors must ship with it. It does not define the storage marker format
that uses it; ADR 0003 and the Linux runtime contract describe that format
once issue #337 adds it.

## Purpose

WyreBox uses libchronoid only to generate and parse the UUIDv7 storage
identifier written into the journal and object store markers by
`wyreboxd --initialize-storage`.

## Acquisition

- Meson first looks for a system `libchronoid` through pkg-config, version
  `>=1.2.0` and `<2`. Distribution packages should provide it this way.
- Without a system package, Meson builds the subproject pinned by
  `subprojects/libchronoid.wrap`: a `wrap-git` of
  `https://github.com/semantic-reasoning/libchronoid.git` at the v1.2.0 commit
  `2188221565f91a67b0c238bb8ad96fd61060c6ee`, with its tests and CLI disabled.
- `-Dwrap_mode=forcefallback` or `-Dforce_fallback_for=libchronoid` always
  uses the subproject.
- The subproject is fetched at configure time and is not committed. For
  offline builds, run `meson subprojects download` while online, or place the
  checkout under `subprojects/` beforehand.
- The Meson summary reports which provider was used.

## Linking and License

libchronoid is licensed under LGPL-3.0-or-later AND MIT; WyreBox is MPL-2.0.

- WyreBox links the libchronoid shared library only, from either provider,
  so users can replace it as LGPL-3.0 section 4(d)(1) requires. Static linking
  is not supported, so a distribution package found through pkg-config must
  provide the shared library.
- Only `wyreboxd` and the tests that exercise storage markers link
  libchronoid. The shared WyreBox library uses its headers only, so the
  Postfix helpers, `wyrebox-admin`, and the Dovecot module do not depend on it.
- WyreBox source files stay under MPL-2.0. libchronoid remains a separate
  component under its own license.
- Redistributors of WyreBox binaries must:
  - ship the libchronoid shared library with its corresponding source or a
    written offer for it;
  - ship its `LICENSE`, `LICENSE.MIT`, and `NOTICE` files, together with the
    GNU GPL v3 and LGPL v3 texts;
  - include a notice that `wyreboxd` uses libchronoid under
    LGPL-3.0-or-later.
- When the subproject is used, `meson install` also installs libchronoid's
  shared and static libraries, headers, pkg-config file, and license files
  into the WyreBox prefix. This path is meant for development and CI;
  packagers should depend on a distribution libchronoid package instead. With
  a prefix whose library directory is not on the dynamic loader path, add it
  through `ld.so.conf` and run `ldconfig`.
