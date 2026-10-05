#pragma once

#include <duckdb.h>
#include <glib.h>

G_BEGIN_DECLS

/*
 * Opens a read-write handle to the process-wide DuckDB instance for @path.
 *
 * Every component in a process that opens the same database file through this
 * function shares one DuckDB instance, so their writes and reads go through a
 * single buffer manager and WAL. The instance stays open until the last handle
 * is closed.
 *
 * NULL, "" and paths starting with ":memory:" open a private in-memory
 * database that is never shared.
 *
 * Read-only opens with a custom configuration must keep using
 * duckdb_open_ext(); the shared instance is always opened with the default
 * configuration.
 *
 * @out_database: (out) (transfer full): on success, a handle the caller must
 *   release with duckdb_close(). Set to NULL on failure.
 */
gboolean wyrebox_duckdb_open_shared (const char *path,
    duckdb_database *out_database, GError **error);

G_END_DECLS
