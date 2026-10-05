#include "wyrebox-duckdb-shared.h"

#include <gio/gio.h>

static duckdb_instance_cache
shared_instance_cache (void)
{
    static gsize initialized = 0;
    static duckdb_instance_cache cache = NULL;

    if (g_once_init_enter (&initialized)) {
        cache = duckdb_create_instance_cache ();
        g_once_init_leave (&initialized, 1);
    }

    return cache;
}

static gboolean
is_memory_path (const char *path)
{
    return path == NULL || *path == '\0' || g_str_has_prefix (path, ":memory:");
}

gboolean
wyrebox_duckdb_open_shared (const char *path, duckdb_database *out_database,
    GError **error)
{
    char *open_error = NULL;

    g_return_val_if_fail (out_database != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    *out_database = NULL;

    if (is_memory_path (path)) {
        if (duckdb_open (NULL, out_database) != DuckDBSuccess) {
            *out_database = NULL;
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "DuckDB in-memory open failed");
            return FALSE;
        }
        return TRUE;
    }

    if (duckdb_get_or_create_from_cache (shared_instance_cache (), path,
        out_database, NULL, &open_error) != DuckDBSuccess) {
        *out_database = NULL;
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "DuckDB open failed for '%s': %s", path,
            open_error != NULL ? open_error : "unknown error");
        if (open_error != NULL)
            duckdb_free (open_error);
        return FALSE;
    }

    return TRUE;
}
