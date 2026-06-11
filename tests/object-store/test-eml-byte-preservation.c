#include "wyrebox-local-object-store.h"

#include <glib.h>
#include <glib/gstdio.h>

static const char *fixture_names[] = {
  "simple-crlf.eml",
  "utf8-8bit-body.eml",
  "multipart-attachment-like.eml",
};

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
assert_fixture_has_crlf_line_endings (const char *name, GBytes *bytes)
{
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (bytes, &size);
  gboolean found_crlf = FALSE;

  for (gsize i = 0; i < size; i++) {
    if (data[i] == '\n') {
      if (i == 0 || data[i - 1] != '\r') {
        g_autofree char *message = g_strdup_printf ("%s contains bare LF at "
            "byte %" G_GSIZE_FORMAT, name, i);

        g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC,
            message);
      }

      found_crlf = TRUE;
    }
  }

  if (!found_crlf) {
    g_autofree char *message = g_strdup_printf ("%s contains no CRLF line "
        "endings", name);

    g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, message);
  }
}

static void
test_eml_fixtures_round_trip_byte_for_byte (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *root = g_dir_make_tmp ("wyrebox-eml-fixtures-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (root);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  for (gsize i = 0; i < G_N_ELEMENTS (fixture_names); i++) {
    g_autoptr (GBytes) input = load_fixture_bytes (fixture_dir,
        fixture_names[i]);
    g_autofree char *key = NULL;
    g_autoptr (GBytes) output = NULL;

    g_test_message ("round-tripping %s", fixture_names[i]);
    assert_fixture_has_crlf_line_endings (fixture_names[i], input);

    g_assert_true (wyrebox_local_object_store_put_bytes (store, input, &key,
            &error));
    g_assert_no_error (error);
    g_assert_nonnull (key);

    output = wyrebox_local_object_store_get_bytes (store, key, &error);
    g_assert_no_error (error);
    g_assert_nonnull (output);
    assert_bytes_equal (output, input);
  }

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/object-store/eml-fixtures-round-trip-byte-for-byte",
      test_eml_fixtures_round_trip_byte_for_byte);

  return g_test_run ();
}
