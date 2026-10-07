#pragma once

#include <duckdb.h>
#include <gio/gio.h>
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
 *
 * Open failures use the G_IO_ERROR code from
 * wyrebox_duckdb_open_error_code().
 */
gboolean wyrebox_duckdb_open_shared (const char *path,
    duckdb_database *out_database, GError **error);

/*
 * Classifies a DuckDB open error message, which is the only detail the DuckDB
 * C API reports: a lock held by another process is G_IO_ERROR_BUSY, a file
 * that is not a valid or intact DuckDB database is G_IO_ERROR_INVALID_DATA,
 * an unsupported storage version is G_IO_ERROR_NOT_SUPPORTED, and anything
 * else, including NULL, is G_IO_ERROR_FAILED.
 *
 * The matched texts are those of the pinned DuckDB release; an unrecognised
 * message deliberately falls back to G_IO_ERROR_FAILED.
 */
GIOErrorEnum wyrebox_duckdb_open_error_code (const char *message);

G_END_DECLS
