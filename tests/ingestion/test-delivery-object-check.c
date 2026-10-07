#include "wyrebox-delivery-object-check.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <string.h>

#define MESSAGE "From: a@example.com\r\nSubject: check\r\n\r\nbody\r\n"

typedef struct
{
    char *root;
    WyreboxLocalObjectStore *store;
    char *object_key;
    char *object_path;
} ObjectFixture;

static void
remove_tree (const char *path)
{
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    const char *name = NULL;

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
object_fixture_clear (ObjectFixture *fixture)
{
    if (fixture->root != NULL)
        remove_tree (fixture->root);
    g_clear_pointer (&fixture->root, g_free);
    g_clear_object (&fixture->store);
    g_clear_pointer (&fixture->object_key, g_free);
    g_clear_pointer (&fixture->object_path, g_free);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (ObjectFixture, object_fixture_clear)
/* *INDENT-ON* */

static void
object_fixture_init (ObjectFixture *fixture)
{
    g_autoptr (GBytes) bytes = g_bytes_new_static (MESSAGE, strlen (MESSAGE));
    g_autoptr (GError) error = NULL;
    const char *hex = NULL;
    g_autofree char *prefix = NULL;
    g_autofree char *filename = NULL;

    fixture->root = g_dir_make_tmp ("wyrebox-object-check-XXXXXX", NULL);
    g_assert_nonnull (fixture->root);
    fixture->store = wyrebox_local_object_store_new (fixture->root, &error);
    g_assert_no_error (error);
    g_assert_true (wyrebox_local_object_store_put_bytes (fixture->store, bytes,
        &fixture->object_key, &error));
    g_assert_no_error (error);

    hex = fixture->object_key + strlen ("sha256:");
    prefix = g_strndup (hex, 2);
    filename = g_strdup_printf ("%s.eml", hex);
    fixture->object_path = g_build_filename (fixture->root, "objects",
            "sha256", prefix, filename, NULL);
    g_assert_true (g_file_test (fixture->object_path, G_FILE_TEST_IS_REGULAR));
}

static void
test_intact_object_is_ok (void)
{
    g_auto (ObjectFixture) fixture = { 0 };
    g_autoptr (GError) error = NULL;

    object_fixture_init (&fixture);

    g_assert_cmpint (wyrebox_delivery_object_check (fixture.store,
        fixture.object_key, strlen (MESSAGE), &error), ==,
        WYREBOX_DELIVERY_OBJECT_CHECK_OK);
    g_assert_no_error (error);
}

static void
test_missing_object (void)
{
    g_auto (ObjectFixture) fixture = { 0 };
    g_autoptr (GError) error = NULL;

    object_fixture_init (&fixture);
    g_assert_cmpint (g_remove (fixture.object_path), ==, 0);

    g_assert_cmpint (wyrebox_delivery_object_check (fixture.store,
        fixture.object_key, strlen (MESSAGE), &error), ==,
        WYREBOX_DELIVERY_OBJECT_CHECK_MISSING);
    g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
}

static void
test_size_mismatch (void)
{
    g_auto (ObjectFixture) fixture = { 0 };
    g_autoptr (GError) error = NULL;

    object_fixture_init (&fixture);

    g_assert_cmpint (wyrebox_delivery_object_check (fixture.store,
        fixture.object_key, strlen (MESSAGE) - 1, &error), ==,
        WYREBOX_DELIVERY_OBJECT_CHECK_SIZE_MISMATCH);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "size"));
}

static void
test_hash_mismatch (void)
{
    g_auto (ObjectFixture) fixture = { 0 };
    g_autoptr (GError) error = NULL;
    g_autofree char *tampered = g_strdup (MESSAGE);

    object_fixture_init (&fixture);
    tampered[0] = 'f';
    g_assert_true (g_file_set_contents (fixture.object_path, tampered, -1,
        &error));
    g_assert_no_error (error);

    g_assert_cmpint (wyrebox_delivery_object_check (fixture.store,
        fixture.object_key, strlen (MESSAGE), &error), ==,
        WYREBOX_DELIVERY_OBJECT_CHECK_HASH_MISMATCH);
    g_assert_error (error, WYREBOX_LOCAL_OBJECT_STORE_ERROR,
        WYREBOX_LOCAL_OBJECT_STORE_ERROR_HASH_MISMATCH);
}

static void
test_unreadable_object_keeps_file_error (void)
{
    g_auto (ObjectFixture) fixture = { 0 };
    g_autoptr (GError) error = NULL;

    object_fixture_init (&fixture);
    g_assert_cmpint (g_remove (fixture.object_path), ==, 0);
    g_assert_cmpint (g_mkdir (fixture.object_path, 0700), ==, 0);

    g_assert_cmpint (wyrebox_delivery_object_check (fixture.store,
        fixture.object_key, strlen (MESSAGE), &error), ==,
        WYREBOX_DELIVERY_OBJECT_CHECK_UNREADABLE);
    g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_ISDIR);
}

static void
test_invalid_key (void)
{
    g_auto (ObjectFixture) fixture = { 0 };
    g_autoptr (GError) error = NULL;

    object_fixture_init (&fixture);

    g_assert_cmpint (wyrebox_delivery_object_check (fixture.store,
        "sha256:not-hex", strlen (MESSAGE), &error), ==,
        WYREBOX_DELIVERY_OBJECT_CHECK_INVALID_KEY);
    g_assert_nonnull (error);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/ingestion/delivery-object-check/intact-object-is-ok",
        test_intact_object_is_ok);
    g_test_add_func ("/ingestion/delivery-object-check/missing-object",
        test_missing_object);
    g_test_add_func ("/ingestion/delivery-object-check/size-mismatch",
        test_size_mismatch);
    g_test_add_func ("/ingestion/delivery-object-check/hash-mismatch",
        test_hash_mismatch);
    g_test_add_func
        ("/ingestion/delivery-object-check/unreadable-object-keeps-file-error",
        test_unreadable_object_keeps_file_error);
    g_test_add_func ("/ingestion/delivery-object-check/invalid-key",
        test_invalid_key);

    return g_test_run ();
}
