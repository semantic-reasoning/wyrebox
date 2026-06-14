#include "wyrebox-local-object-store.h"

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

static char *
object_path_for_key (const char *root_dir, const char *object_key)
{
  const char *hex = object_key + strlen ("sha256:");
  g_autofree char *prefix = g_strndup (hex, 2);
  g_autofree char *filename = g_strdup_printf ("%s.eml", hex);

  return g_build_filename (root_dir,
      "objects", "sha256", prefix, filename, NULL);
}

static char *
object_path_for_bytes (const char *root_dir, GBytes *bytes)
{
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (bytes, &size);
  g_autofree char *hex =
      g_compute_checksum_for_data (G_CHECKSUM_SHA256, data, size);
  g_autofree char *key = g_strdup_printf ("sha256:%s", hex);

  return object_path_for_key (root_dir, key);
}

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

static void
assert_bytes_equal (GBytes *actual, const guint8 *expected, gsize expected_size)
{
  gsize actual_size = 0;
  const guint8 *actual_data = g_bytes_get_data (actual, &actual_size);

  g_assert_cmpuint (actual_size, ==, expected_size);
  g_assert_cmpmem (actual_data, actual_size, expected, expected_size);
}

static void
test_round_trip_preserves_bytes (void)
{
  const guint8 message[] = "From: a@example.test\r\n\r\nhello\r\n";
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (GBytes) input = g_bytes_new_static (message, sizeof (message) - 1);
  g_autofree char *key = NULL;
  g_autoptr (GBytes) output = NULL;

  g_assert_nonnull (root);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_local_object_store_put_bytes (store, input, &key,
          &error));
  g_assert_no_error (error);
  g_assert_nonnull (key);
  g_assert_true (g_regex_match_simple ("^sha256:[0-9a-f]{64}$", key, 0, 0));

  output = wyrebox_local_object_store_get_bytes (store, key, &error);
  g_assert_no_error (error);
  g_assert_nonnull (output);
  assert_bytes_equal (output, message, sizeof (message) - 1);

  remove_tree (root);
}

static void
test_duplicate_put_is_idempotent_and_does_not_rewrite (void)
{
  const guint8 message[] = "Subject: duplicate\r\n\r\nsame bytes\r\n";
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (GBytes) input = g_bytes_new_static (message, sizeof (message) - 1);
  g_autofree char *first_key = NULL;
  g_autofree char *second_key = NULL;
  g_autofree char *path = NULL;
  GStatBuf before = { 0 };
  GStatBuf after = { 0 };

  g_assert_nonnull (root);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_local_object_store_put_bytes (store, input, &first_key,
          &error));
  g_assert_no_error (error);
  path = object_path_for_key (root, first_key);
  g_assert_cmpint (g_stat (path, &before), ==, 0);

  g_assert_true (wyrebox_local_object_store_put_bytes (store, input,
          &second_key, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (second_key, ==, first_key);
  g_assert_cmpint (g_stat (path, &after), ==, 0);
  g_assert_cmpint (after.st_ino, ==, before.st_ino);
  g_assert_cmpint (after.st_mtime, ==, before.st_mtime);
  g_assert_cmpint (after.st_size, ==, before.st_size);

  remove_tree (root);
}

static void
test_put_creates_sharded_object_without_temp_leftover (void)
{
  const guint8 message[] = "Subject: layout\r\n\r\nstored once\r\n";
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (GBytes) input = g_bytes_new_static (message, sizeof (message) - 1);
  g_autofree char *key = NULL;
  g_autofree char *path = NULL;
  g_autofree char *parent_dir = NULL;
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;

  g_assert_nonnull (root);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_local_object_store_put_bytes (store, input, &key,
          &error));
  g_assert_no_error (error);
  path = object_path_for_key (root, key);
  parent_dir = g_path_get_dirname (path);
  g_assert_true (g_file_test (path, G_FILE_TEST_IS_REGULAR));

  dir = g_dir_open (parent_dir, 0, &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);
  while ((entry = g_dir_read_name (dir)) != NULL)
    g_assert_false (g_str_has_prefix (entry, ".tmp-object-"));

  remove_tree (root);
}

static void
test_different_bytes_produce_different_keys (void)
{
  const guint8 first[] = "Subject: one\r\n\r\none\r\n";
  const guint8 second[] = "Subject: two\r\n\r\ntwo\r\n";
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (GBytes) first_bytes =
      g_bytes_new_static (first, sizeof (first) - 1);
  g_autoptr (GBytes) second_bytes =
      g_bytes_new_static (second, sizeof (second) - 1);
  g_autofree char *first_key = NULL;
  g_autofree char *second_key = NULL;

  g_assert_nonnull (root);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_local_object_store_put_bytes (store, first_bytes,
          &first_key, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_local_object_store_put_bytes (store, second_bytes,
          &second_key, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (first_key, !=, second_key);

  remove_tree (root);
}

static void
test_invalid_key_fails (void)
{
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (GBytes) output = NULL;

  g_assert_nonnull (root);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  output =
      wyrebox_local_object_store_get_bytes (store, "sha256:not-hex", &error);
  g_assert_null (output);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  remove_tree (root);
}

static void
test_missing_valid_key_fails (void)
{
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (GBytes) output = NULL;
  const char *missing_key =
      "sha256:0000000000000000000000000000000000000000000000000000000000000000";

  g_assert_nonnull (root);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  output = wyrebox_local_object_store_get_bytes (store, missing_key, &error);
  g_assert_null (output);
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT);

  remove_tree (root);
}

static void
test_open_existing_reads_initialized_store (void)
{
  const guint8 message[] = "Subject: existing\r\n\r\nstored bytes\r\n";
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) writer = NULL;
  g_autoptr (WyreboxLocalObjectStore) reader = NULL;
  g_autoptr (GBytes) input = g_bytes_new_static (message, sizeof (message) - 1);
  g_autofree char *key = NULL;
  g_autoptr (GBytes) output = NULL;

  g_assert_nonnull (root);
  writer = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  g_assert_true (wyrebox_local_object_store_put_bytes (writer, input, &key,
          &error));
  g_assert_no_error (error);

  reader = wyrebox_local_object_store_open_existing (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  output = wyrebox_local_object_store_get_bytes (reader, key, &error);
  g_assert_no_error (error);
  g_assert_nonnull (output);
  assert_bytes_equal (output, message, sizeof (message) - 1);

  remove_tree (root);
}

static void
test_open_existing_rejects_uninitialized_store_without_creating_dirs (void)
{
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autofree char *objects_dir = NULL;

  g_assert_nonnull (root);
  objects_dir = g_build_filename (root, "objects", "sha256", NULL);
  g_assert_false (g_file_test (objects_dir, G_FILE_TEST_EXISTS));

  store = wyrebox_local_object_store_open_existing (root, &error);
  g_assert_null (store);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_false (g_file_test (objects_dir, G_FILE_TEST_EXISTS));

  remove_tree (root);
}

static void
test_open_existing_rejects_put_without_creating_object (void)
{
  const guint8 message[] = "Subject: read-only\r\n\r\nmust not store\r\n";
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) writer = NULL;
  g_autoptr (WyreboxLocalObjectStore) reader = NULL;
  g_autoptr (GBytes) input = g_bytes_new_static (message, sizeof (message) - 1);
  g_autofree char *key = g_strdup ("unchanged");
  g_autofree char *path = NULL;
  g_autofree char *parent_dir = NULL;

  g_assert_nonnull (root);
  writer = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  path = object_path_for_bytes (root, input);
  parent_dir = g_path_get_dirname (path);
  g_assert_false (g_file_test (parent_dir, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  reader = wyrebox_local_object_store_open_existing (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  g_assert_false (wyrebox_local_object_store_put_bytes (reader, input, &key,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpstr (key, ==, "unchanged");
  g_assert_false (g_file_test (parent_dir, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  remove_tree (root);
}

static void
test_fetch_rejects_corrupted_object (void)
{
  const guint8 message[] = "Subject: integrity\r\n\r\noriginal bytes\r\n";
  const char replacement[] = "Subject: integrity\r\n\r\ncorrupted bytes\r\n";
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (GBytes) input = g_bytes_new_static (message, sizeof (message) - 1);
  g_autofree char *key = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GBytes) output = NULL;

  g_assert_nonnull (root);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_local_object_store_put_bytes (store, input, &key,
          &error));
  g_assert_no_error (error);
  path = object_path_for_key (root, key);
  g_assert_true (g_file_set_contents (path, replacement, strlen (replacement),
          &error));
  g_assert_no_error (error);

  output = wyrebox_local_object_store_get_bytes (store, key, &error);
  g_assert_null (output);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  remove_tree (root);
}

static void
test_fetch_rejects_truncated_object (void)
{
  const guint8 message[] = "Subject: integrity\r\n\r\noriginal bytes\r\n";
  const char replacement[] = "Subject: integrity\r\n";
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (GBytes) input = g_bytes_new_static (message, sizeof (message) - 1);
  g_autofree char *key = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GBytes) output = NULL;

  g_assert_nonnull (root);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_local_object_store_put_bytes (store, input, &key,
          &error));
  g_assert_no_error (error);
  path = object_path_for_key (root, key);
  g_assert_true (g_file_set_contents (path, replacement, strlen (replacement),
          &error));
  g_assert_no_error (error);

  output = wyrebox_local_object_store_get_bytes (store, key, &error);
  g_assert_null (output);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  remove_tree (root);
}

static void
test_duplicate_put_rejects_corrupted_existing_object (void)
{
  const guint8 message[] = "Subject: duplicate\r\n\r\nsame bytes\r\n";
  const char replacement[] = "Subject: duplicate\r\n\r\ncorrupt bytes\r\n";
  g_autofree char *root = g_dir_make_tmp ("wyrebox-object-store-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (GBytes) input = g_bytes_new_static (message, sizeof (message) - 1);
  g_autofree char *first_key = NULL;
  g_autofree char *second_key = NULL;
  g_autofree char *path = NULL;

  g_assert_nonnull (root);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_local_object_store_put_bytes (store, input, &first_key,
          &error));
  g_assert_no_error (error);
  path = object_path_for_key (root, first_key);
  g_assert_true (g_file_set_contents (path, replacement, strlen (replacement),
          &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_local_object_store_put_bytes (store, input,
          &second_key, &error));
  g_assert_null (second_key);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/object-store/round-trip-preserves-bytes",
      test_round_trip_preserves_bytes);
  g_test_add_func ("/object-store/duplicate-put-is-idempotent",
      test_duplicate_put_is_idempotent_and_does_not_rewrite);
  g_test_add_func
      ("/object-store/put-creates-sharded-object-without-temp-leftover",
      test_put_creates_sharded_object_without_temp_leftover);
  g_test_add_func ("/object-store/different-bytes-produce-different-keys",
      test_different_bytes_produce_different_keys);
  g_test_add_func ("/object-store/invalid-key-fails", test_invalid_key_fails);
  g_test_add_func ("/object-store/missing-valid-key-fails",
      test_missing_valid_key_fails);
  g_test_add_func ("/object-store/open-existing-reads-initialized-store",
      test_open_existing_reads_initialized_store);
  g_test_add_func
      ("/object-store/open-existing-rejects-uninitialized-without-creating-dirs",
      test_open_existing_rejects_uninitialized_store_without_creating_dirs);
  g_test_add_func
      ("/object-store/open-existing-rejects-put-without-creating-object",
      test_open_existing_rejects_put_without_creating_object);
  g_test_add_func ("/object-store/fetch-rejects-corrupted-object",
      test_fetch_rejects_corrupted_object);
  g_test_add_func ("/object-store/fetch-rejects-truncated-object",
      test_fetch_rejects_truncated_object);
  g_test_add_func ("/object-store/duplicate-put-rejects-corrupted-existing",
      test_duplicate_put_rejects_corrupted_existing_object);

  return g_test_run ();
}
