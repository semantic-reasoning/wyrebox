#include "wyrebox-eml-ingestor.h"
#include "wyrebox-local-object-store.h"

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
assert_bytes_equal (GBytes *actual, GBytes *expected)
{
  gsize actual_size = 0;
  gsize expected_size = 0;
  const guint8 *actual_data = g_bytes_get_data (actual, &actual_size);
  const guint8 *expected_data = g_bytes_get_data (expected, &expected_size);

  g_assert_cmpuint (actual_size, ==, expected_size);
  g_assert_cmpmem (actual_data, actual_size, expected_data, expected_size);
}

static void
test_ingests_raw_fixture_into_object_store (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *root = g_dir_make_tmp ("wyrebox-eml-ingestor-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) output = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  gsize input_size = 0;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  input_size = g_bytes_get_size (input);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  ingestor = wyrebox_eml_ingestor_new (store);
  g_assert_nonnull (ingestor);

  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
          &result, &error));
  g_assert_no_error (error);
  g_assert_nonnull (result.object_key);
  g_assert_true (g_regex_match_simple ("^sha256:[0-9a-f]{64}$",
          result.object_key, 0, 0));
  g_assert_cmpuint (result.size_bytes, ==, input_size);

  output = wyrebox_local_object_store_get_bytes (store, result.object_key,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (output);
  assert_bytes_equal (output, input);

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ingestion/eml-ingestor/raw-fixture-into-object-store",
      test_ingests_raw_fixture_into_object_store);

  return g_test_run ();
}
