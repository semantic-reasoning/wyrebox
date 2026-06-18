#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"

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
  gint64 elapsed_us = 0;

  g_assert_nonnull (fixture_dir);

  root = g_dir_make_tmp ("wyrebox-benchmark-eml-ingestor-XXXXXX", NULL);
  g_assert_nonnull (root);

  {
    g_autofree char *object_root = g_build_filename (root, "objects", NULL);
    g_autofree char *journal_root = g_build_filename (root, "journal", NULL);
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_autoptr (GBytes) input = NULL;
    g_auto (WyreboxEmlIngestResult) result = { 0 };
    gint64 start_us = 0;
    gint64 end_us = 0;

    object_store = wyrebox_local_object_store_new (object_root, &error);
    g_assert_no_error (error);
    g_assert_nonnull (object_store);

    writer = wyrebox_journal_writer_new (journal_root, &error);
    g_assert_no_error (error);
    g_assert_nonnull (writer);

    ingestor = wyrebox_eml_ingestor_new_with_journal (object_store, writer);
    g_assert_nonnull (ingestor);

    input = load_fixture_bytes (fixture_dir, fixture_name);

    start_us = g_get_monotonic_time ();
    g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input, &result,
            &error));
    end_us = g_get_monotonic_time ();
    g_assert_no_error (error);
    g_assert_nonnull (result.object_key);

    elapsed_us = end_us - start_us;

    g_print ("{\"schema\":\"wyrebox-benchmark-report/v1\",");
    g_print ("\"suite\":\"ingestion\",");
    g_print ("\"case\":\"eml-ingestor\",");
    g_print ("\"fixture\":\"%s\",", fixture_name);
    g_print ("\"metric\":\"elapsed_us\",");
    g_print ("\"value\":%" G_GINT64_FORMAT ",", elapsed_us);
    g_print ("\"status\":\"ok\"}\n");
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
