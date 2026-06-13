# DuckDB Dependency Contract

## Status

Accepted.

## Scope

This contract defines only DuckDB acquisition policy during Meson configuration.
It intentionally covers wrap-backed dependency selection and platform constraints,
but does not define query plan behavior, schema migration policy, or runtime API
semantics.

## Dependency Policy

DuckDB is required for the build. Build configuration must fail when DuckDB
cannot be acquired via the expected path.

DuckDB must be acquired through the tracked wrap file
`subprojects/duckdb-prebuilt-linux.wrap` and the direct
`subproject('duckdb-prebuilt-linux')` call.

System `duckdb` providers resolved via pkg-config, direct
`dependency('duckdb', ...)`, or provider fallback are not accepted.

The direct subproject path is intentional because Meson dependency fallback can
resolve system providers and bypass the tracked prebuilt acquisition contract.

WyreBox uses the dependency from the subproject (`duckdb_subproject_dep`) for all
DuckDB links.

## Platform Constraint

The dependency policy is intentionally constrained to the Linux x86_64 prebuilt
artifact.

## Wrap Contract

The tracked wrap filename is `duckdb-prebuilt-linux.wrap`; it must remain named as
`subprojects/duckdb-prebuilt-linux.wrap` and provides
`duckdb-prebuilt-linux = duckdb_dep`.

No `duckdb.wrap` file is used.

## Commit Hygiene

This contract forbids committing generated or extracted third-party subproject
directories.

For the DuckDB acquisition path, only `subprojects/duckdb-prebuilt-linux.wrap`
and `subprojects/packagefiles/duckdb-prebuilt-linux/meson.build` are committed.
