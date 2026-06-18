#include "wyrebox-local-object-store.h"

#include <string.h>

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

static char *
object_path_for_key (const char *root_dir, const char *object_key)
{
  const char *hex = object_key + strlen ("sha256:");
  g_autofree char *prefix = g_strndup (hex, 2);
  g_autofree char *filename = g_strdup_printf ("%s.eml", hex);

  return g_build_filename (root_dir,
      "objects", "sha256", prefix, filename, NULL);
}

static void
run_microbenchmark (void)
{
  static const guint8 payload_data[] =
      "From: sender@example.test\r\n"
      "To: recipient@example.test\r\n"
      "Subject: wyrebox object-store get benchmark\r\n"
      "Date: Thu, 01 Jan 1970 00:00:00 +0000\r\n"
      "Message-ID: <wyrebox-object-store-get-benchmark@example.test>\r\n"
      "\r\n" "fixed RFC 5322 payload\r\n";
  g_autofree char *root = NULL;
  gint64 elapsed_us = 0;

  root = g_dir_make_tmp ("wyrebox-benchmark-object-store-get-XXXXXX", NULL);
  g_assert_nonnull (root);

  {
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (GBytes) payload = NULL;
    g_autoptr (GBytes) fetched = NULL;
    g_autofree char *object_key = NULL;
    g_autofree char *expected_key = NULL;
    g_autofree char *object_path = NULL;
    gsize payload_size = 0;
    const guint8 *payload_bytes = NULL;
    gint64 start_us = 0;
    gint64 end_us = 0;

    object_store = wyrebox_local_object_store_new (root, &error);
    g_assert_no_error (error);
    g_assert_nonnull (object_store);

    payload = g_bytes_new_static (payload_data, sizeof (payload_data) - 1);
    payload_bytes = g_bytes_get_data (payload, &payload_size);
    {
      g_autofree char *checksum =
          g_compute_checksum_for_data (G_CHECKSUM_SHA256,
          payload_bytes, payload_size);

      expected_key = g_strdup_printf ("sha256:%s", checksum);
    }

    g_assert_true (wyrebox_local_object_store_put_bytes (object_store,
            payload, &object_key, &error));
    g_assert_no_error (error);
    g_assert_nonnull (object_key);
    g_assert_cmpstr (object_key, ==, expected_key);
    g_assert_true (g_regex_match_simple ("^sha256:[0-9a-f]{64}$", object_key,
            0, 0));

    object_path = object_path_for_key (root, object_key);
    g_assert_true (g_file_test (object_path, G_FILE_TEST_IS_REGULAR));

    start_us = g_get_monotonic_time ();
    fetched = wyrebox_local_object_store_get_bytes (object_store, object_key,
        &error);
    end_us = g_get_monotonic_time ();
    g_assert_no_error (error);
    g_assert_nonnull (fetched);

    elapsed_us = end_us - start_us;

    g_assert_true (g_bytes_equal (payload, fetched));
    g_assert_cmpuint (g_bytes_get_size (fetched), ==, payload_size);

    g_print ("{\"schema\":\"wyrebox-benchmark-report/v1\",");
    g_print ("\"suite\":\"object-store\",");
    g_print ("\"case\":\"get-bytes\",");
    g_print ("\"metric\":\"elapsed_us\",");
    g_print ("\"value\":%" G_GINT64_FORMAT ",", elapsed_us);
    g_print ("\"status\":\"ok\"}\n");
  }

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  run_microbenchmark ();

  return 0;
}
