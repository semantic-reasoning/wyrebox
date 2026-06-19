#include "wyrebox-local-object-store.h"
#include "wyrebox-benchmark-report.h"

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
    (void) g_remove (path);
    return;
  }

  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, name, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

static GBytes *
load_fixture_bytes (const char *fixture_dir, const char *name)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *path = g_build_filename (fixture_dir, name, NULL);
  g_autofree char *contents = NULL;
  gsize length = 0;

  g_assert_true (g_file_get_contents (path, &contents, &length, &error));
  g_assert_no_error (error);

  return g_bytes_new_take (g_steal_pointer (&contents), length);
}

static void
run_microbenchmark (const char *fixture_dir)
{
  static const char *fixture_name = "simple-crlf.eml";
  g_autofree char *root = NULL;
  g_auto (WyreboxBenchmarkReport) report = { 0 };

  g_assert_nonnull (fixture_dir);

  root = g_dir_make_tmp ("wyrebox-benchmark-object-store-XXXXXX", NULL);
  g_assert_nonnull (root);

  {
    g_autofree char *object_root = g_build_filename (root, "objects", NULL);
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (GBytes) input = NULL;
    g_autoptr (GBytes) stored = NULL;
    g_autofree char *object_key = NULL;
    gint64 start_us = 0;
    gint64 end_us = 0;

    object_store = wyrebox_local_object_store_new (object_root, &error);
    g_assert_no_error (error);
    g_assert_nonnull (object_store);

    input = load_fixture_bytes (fixture_dir, fixture_name);

    wyrebox_benchmark_report_init (&report);
    start_us = g_get_monotonic_time ();
    g_assert_true (wyrebox_local_object_store_put_bytes (object_store,
            input, &object_key, &error));
    end_us = g_get_monotonic_time ();
    g_assert_no_error (error);
    g_assert_nonnull (object_key);

    report.elapsed_us = end_us - start_us;
    wyrebox_benchmark_report_capture_rusage (&report);

    stored = wyrebox_local_object_store_get_bytes (object_store,
        object_key, &error);
    g_assert_no_error (error);
    g_assert_nonnull (stored);

    g_assert_true (g_bytes_equal (input, stored));
    g_assert_cmpuint (g_bytes_get_size (stored), ==, g_bytes_get_size (input));
    report.object_count = 1;
    report.disk_bytes = g_bytes_get_size (stored);

    wyrebox_benchmark_report_print_json ("object-store", "put-bytes", &report);
  }

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  const char *fixture_dir = NULL;

  (void) argc;
  (void) argv;

  fixture_dir = g_getenv ("WYREBOX_BENCHMARK_FIXTURE_DIR");
  if (fixture_dir == NULL) {
    g_printerr ("WYREBOX_BENCHMARK_FIXTURE_DIR is required\n");
    return 1;
  }

  run_microbenchmark (fixture_dir);

  return 0;
}
