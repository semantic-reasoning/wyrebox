#include "wyrebox-duckdb-shared.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

static void
remove_tree (const char *path)
{
    g_autoptr (GDir) dir = NULL;
    const char *name = NULL;

    dir = g_dir_open (path, 0, NULL);
    if (dir == NULL) {
        (void)g_remove (path);
        return;
    }

    while ((name = g_dir_read_name (dir)) != NULL) {
        g_autofree char *child = g_build_filename (path, name, NULL);

        remove_tree (child);
    }

    (void)g_rmdir (path);
}

static void
exec_sql (duckdb_database database, const char *sql)
{
    duckdb_connection connection = NULL;
    duckdb_result result;

    g_assert_cmpint (duckdb_connect (database, &connection), ==,
        DuckDBSuccess);
    if (duckdb_query (connection, sql, &result) != DuckDBSuccess)
        g_error ("query failed: %s: %s", sql, duckdb_result_error (&result));
    duckdb_destroy_result (&result);
    duckdb_disconnect (&connection);
}

static gint64
count_rows (duckdb_database database, const char *table)
{
    g_autofree char *sql = g_strdup_printf ("SELECT count(*) FROM %s", table);
    duckdb_connection connection = NULL;
    duckdb_result result;
    gint64 count = -1;

    g_assert_cmpint (duckdb_connect (database, &connection), ==,
        DuckDBSuccess);
    if (duckdb_query (connection, sql, &result) == DuckDBSuccess)
        count = duckdb_value_int64 (&result, 0, 0);
    duckdb_destroy_result (&result);
    duckdb_disconnect (&connection);

    return count;
}

static void
test_handles_share_one_instance (void)
{
    g_autofree char *root = g_dir_make_tmp ("wyrebox-duckdb-shared-XXXXXX",
            NULL);
    g_autofree char *path = g_build_filename (root, "catalog.duckdb", NULL);
    g_autoptr (GError) error = NULL;
    duckdb_database first = NULL;
    duckdb_database second = NULL;
    duckdb_database reopened = NULL;

    g_assert_true (wyrebox_duckdb_open_shared (path, &first, &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_duckdb_open_shared (path, &second, &error));
    g_assert_no_error (error);

    exec_sql (first, "CREATE TABLE t (v INTEGER)");
    exec_sql (first, "INSERT INTO t VALUES (1)");
    g_assert_cmpint (count_rows (second, "t"), ==, 1);

    duckdb_close (&first);
    exec_sql (second, "INSERT INTO t VALUES (2)");
    g_assert_cmpint (count_rows (second, "t"), ==, 2);
    duckdb_close (&second);

    g_assert_true (wyrebox_duckdb_open_shared (path, &reopened, &error));
    g_assert_no_error (error);
    g_assert_cmpint (count_rows (reopened, "t"), ==, 2);
    duckdb_close (&reopened);

    remove_tree (root);
}

static void
test_memory_databases_are_private (void)
{
    g_autoptr (GError) error = NULL;
    duckdb_database first = NULL;
    duckdb_database second = NULL;
    duckdb_database third = NULL;

    g_assert_true (wyrebox_duckdb_open_shared (":memory:", &first, &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_duckdb_open_shared (":memory:", &second, &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_duckdb_open_shared (NULL, &third, &error));
    g_assert_no_error (error);

    exec_sql (first, "CREATE TABLE t (v INTEGER)");
    g_assert_cmpint (count_rows (second, "t"), ==, -1);
    g_assert_cmpint (count_rows (third, "t"), ==, -1);

    duckdb_close (&first);
    duckdb_close (&second);
    duckdb_close (&third);
}

static void
test_open_failure_reports_error (void)
{
    g_autofree char *root = g_dir_make_tmp ("wyrebox-duckdb-shared-XXXXXX",
            NULL);
    g_autofree char *path = g_build_filename (root, "missing", "dir",
            "catalog.duckdb", NULL);
    g_autoptr (GError) error = NULL;
    duckdb_database database = NULL;

    g_assert_false (wyrebox_duckdb_open_shared (path, &database, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_null (database);

    remove_tree (root);
}

static void
test_open_error_code_classifies_messages (void)
{
    g_assert_cmpint (wyrebox_duckdb_open_error_code ("IO Error: Could not set "
        "lock on file \"/x/catalog.duckdb\": Conflicting lock is held in "
        "/usr/bin/wyreboxd (PID 7)"), ==, G_IO_ERROR_BUSY);
    g_assert_cmpint (wyrebox_duckdb_open_error_code ("IO Error: The file "
        "\"/x/catalog.duckdb\" exists, but it is not a valid DuckDB "
        "database file!"), ==, G_IO_ERROR_INVALID_DATA);
    g_assert_cmpint (wyrebox_duckdb_open_error_code ("Corrupt database file: "
        "computed checksum 1 does not match stored checksum 2 in block "
        "at location 4096"), ==, G_IO_ERROR_INVALID_DATA);
    g_assert_cmpint (wyrebox_duckdb_open_error_code ("IO Error: Trying to "
        "read a database file with version number 99, but we can only "
        "read versions between 1 and 67."), ==, G_IO_ERROR_NOT_SUPPORTED);
    g_assert_cmpint (wyrebox_duckdb_open_error_code ("IO Error: Cannot open "
        "file \"/x/catalog.duckdb\": No such file or directory"), ==,
        G_IO_ERROR_FAILED);
    g_assert_cmpint (wyrebox_duckdb_open_error_code (NULL), ==,
        G_IO_ERROR_FAILED);
}

static void
test_garbage_file_reports_invalid_data (void)
{
    g_autofree char *root = g_dir_make_tmp ("wyrebox-duckdb-shared-XXXXXX",
            NULL);
    g_autofree char *path = g_build_filename (root, "catalog.duckdb", NULL);
    g_autofree char *garbage = g_strnfill (8192, 'x');
    g_autoptr (GError) error = NULL;
    duckdb_database database = NULL;

    g_assert_true (g_file_set_contents (path, garbage, 8192, &error));
    g_assert_no_error (error);

    g_assert_false (wyrebox_duckdb_open_shared (path, &database, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_null (database);

    remove_tree (root);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/duckdb/shared/handles-share-one-instance",
        test_handles_share_one_instance);
    g_test_add_func ("/duckdb/shared/memory-databases-are-private",
        test_memory_databases_are_private);
    g_test_add_func ("/duckdb/shared/open-failure-reports-error",
        test_open_failure_reports_error);
    g_test_add_func ("/duckdb/shared/open-error-code-classifies-messages",
        test_open_error_code_classifies_messages);
    g_test_add_func ("/duckdb/shared/garbage-file-reports-invalid-data",
        test_garbage_file_reports_invalid_data);

    return g_test_run ();
}
