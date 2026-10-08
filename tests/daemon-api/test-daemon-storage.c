#include "wyrebox-daemon-storage.h"
#include "wyrebox-journal-reader.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define STORAGE_ID_A "01920000-0000-7000-8000-000000000001"
#define STORAGE_ID_B "01920000-0000-7000-8000-000000000002"

typedef struct
{
    char *root;
    char *journal_root;
    char *object_root;
} StorageFixture;

static void
remove_tree (const char *path)
{
    g_autoptr (GDir) dir = NULL;
    const char *name = NULL;

    if (!g_file_test (path, G_FILE_TEST_IS_DIR)
        || g_file_test (path, G_FILE_TEST_IS_SYMLINK)) {
        (void)g_remove (path);
        return;
    }

    (void)g_chmod (path, 0700);
    dir = g_dir_open (path, 0, NULL);
    while (dir != NULL && (name = g_dir_read_name (dir)) != NULL) {
        g_autofree char *child = g_build_filename (path, name, NULL);

        remove_tree (child);
    }
    (void)g_rmdir (path);
}

static void
storage_fixture_set_up (StorageFixture *fixture, gconstpointer user_data)
{
    (void)user_data;

    fixture->root = g_dir_make_tmp ("wyrebox-daemon-storage-XXXXXX", NULL);
    g_assert_nonnull (fixture->root);
    fixture->journal_root = g_build_filename (fixture->root, "journal", NULL);
    fixture->object_root = g_build_filename (fixture->root, "object-store",
            NULL);
}

static void
storage_fixture_tear_down (StorageFixture *fixture, gconstpointer user_data)
{
    (void)user_data;

    remove_tree (fixture->root);
    g_clear_pointer (&fixture->root, g_free);
    g_clear_pointer (&fixture->journal_root, g_free);
    g_clear_pointer (&fixture->object_root, g_free);
}

static char *
journal_marker_path (const StorageFixture *fixture)
{
    return g_build_filename (fixture->journal_root,
               WYREBOX_DAEMON_STORAGE_JOURNAL_MARKER, NULL);
}

static char *
object_marker_path (const StorageFixture *fixture)
{
    return g_build_filename (fixture->object_root,
               WYREBOX_DAEMON_STORAGE_OBJECT_STORE_MARKER, NULL);
}

static void
make_roots (const StorageFixture *fixture)
{
    g_assert_cmpint (g_mkdir_with_parents (fixture->journal_root, 0700), ==,
        0);
    g_assert_cmpint (g_mkdir_with_parents (fixture->object_root, 0700), ==, 0);
}

static void
write_file (const char *path, const char *contents, gssize length)
{
    g_autoptr (GError) error = NULL;

    g_assert_true (g_file_set_contents (path, contents, length, &error));
    g_assert_no_error (error);
}

static char *
read_file (const char *path)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *contents = NULL;

    g_assert_true (g_file_get_contents (path, &contents, NULL, &error));
    g_assert_no_error (error);
    return g_steal_pointer (&contents);
}

static char *
marker_text (const char *role, const char *storage_id)
{
    return g_strdup_printf ("[WyreBox Storage]\nformat=1\nrole=%s\n"
               "storage_id=%s\n", role, storage_id);
}

static void
write_journal_marker (const StorageFixture *fixture, const char *contents)
{
    g_autofree char *path = journal_marker_path (fixture);

    write_file (path, contents, -1);
}

static void
write_object_marker (const StorageFixture *fixture, const char *contents)
{
    g_autofree char *path = object_marker_path (fixture);

    write_file (path, contents, -1);
}

static void
write_valid_markers (const StorageFixture *fixture, const char *journal_id,
    const char *object_id)
{
    g_autofree char *journal = marker_text ("journal", journal_id);
    g_autofree char *object = marker_text ("object-store", object_id);

    make_roots (fixture);
    write_journal_marker (fixture, journal);
    write_object_marker (fixture, object);
}

static char *
segment_path (const StorageFixture *fixture)
{
    return g_build_filename (fixture->journal_root,
               WYREBOX_JOURNAL_SEGMENT_NAME, NULL);
}

static void
write_journal_data (const StorageFixture *fixture, gssize length)
{
    g_autofree char *path = segment_path (fixture);

    g_assert_cmpint (g_mkdir_with_parents (fixture->journal_root, 0700), ==,
        0);
    write_file (path, "WYREJNL1-journal-bytes", length);
}

static char *
object_data_path (const StorageFixture *fixture)
{
    return g_build_filename (fixture->object_root, "objects", "sha256", "ab",
               "abcdef.eml", NULL);
}

static void
write_object_data (const StorageFixture *fixture)
{
    g_autofree char *path = object_data_path (fixture);
    g_autofree char *dir = g_path_get_dirname (path);

    g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
    write_file (path, "raw object bytes", -1);
}

static gboolean
objects_dir_exists (const StorageFixture *fixture)
{
    g_autofree char *path = g_build_filename (fixture->object_root, "objects",
            "sha256", NULL);

    return g_file_test (path, G_FILE_TEST_IS_DIR);
}

static gboolean
any_marker_exists (const StorageFixture *fixture)
{
    g_autofree char *journal = journal_marker_path (fixture);
    g_autofree char *object = object_marker_path (fixture);

    return g_file_test (journal, G_FILE_TEST_EXISTS) ||
           g_file_test (object, G_FILE_TEST_EXISTS);
}

static char *
marker_storage_id (const char *path)
{
    g_autoptr (GKeyFile) key_file = g_key_file_new ();
    g_autoptr (GError) error = NULL;
    char *storage_id = NULL;

    g_assert_true (g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE,
        &error));
    g_assert_no_error (error);
    storage_id = g_key_file_get_string (key_file, "WyreBox Storage",
            "storage_id", &error);
    g_assert_no_error (error);
    return storage_id;
}

static void
assert_check_fails (const StorageFixture *fixture, GQuark domain, gint code,
    const char *substring)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *storage_id = NULL;

    g_assert_false (wyrebox_daemon_storage_check_initialized
            (fixture->journal_root, fixture->object_root, &storage_id, &error));
    g_assert_error (error, domain, code);
    g_assert_null (storage_id);
    if (substring != NULL)
        g_assert_nonnull (strstr (error->message, substring));
}

static void
assert_initialize_fails (const StorageFixture *fixture, GQuark domain,
    gint code, const char *substring)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *storage_id = NULL;
    gboolean already = TRUE;

    g_assert_false (wyrebox_daemon_storage_initialize (fixture->journal_root,
        fixture->object_root, &already, &storage_id, &error));
    g_assert_error (error, domain, code);
    g_assert_null (storage_id);
    if (substring != NULL)
        g_assert_nonnull (strstr (error->message, substring));
}

static char *
initialize_ok (const StorageFixture *fixture, gboolean expect_already)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *storage_id = NULL;
    gboolean already = !expect_already;

    g_assert_true (wyrebox_daemon_storage_initialize (fixture->journal_root,
        fixture->object_root, &already, &storage_id, &error));
    g_assert_no_error (error);
    g_assert_cmpint (already, ==, expect_already);
    g_assert_nonnull (storage_id);
    return g_steal_pointer (&storage_id);
}

static void
assert_checks_with_id (const StorageFixture *fixture, const char *expected)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *storage_id = NULL;

    g_assert_true (wyrebox_daemon_storage_check_initialized
            (fixture->journal_root, fixture->object_root, &storage_id, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (storage_id, ==, expected);
}

static void
test_check_fails_when_roots_are_missing (StorageFixture *fixture,
    gconstpointer user_data)
{
    (void)user_data;

    assert_check_fails (fixture, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        "storage is not initialized");
    g_assert_false (g_file_test (fixture->journal_root, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (fixture->object_root, G_FILE_TEST_EXISTS));
}

static void
test_check_fails_when_roots_are_empty (StorageFixture *fixture,
    gconstpointer user_data)
{
    (void)user_data;

    make_roots (fixture);
    assert_check_fails (fixture, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        "run wyreboxd --initialize-storage");
    g_assert_false (objects_dir_exists (fixture));
    g_assert_false (any_marker_exists (fixture));
}

static void
test_check_fails_when_one_marker_is_missing (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *journal = marker_text ("journal", STORAGE_ID_A);
    g_autofree char *object = marker_text ("object-store", STORAGE_ID_A);
    g_autofree char *journal_path = journal_marker_path (fixture);
    g_autofree char *object_path = object_marker_path (fixture);

    (void)user_data;

    make_roots (fixture);
    write_journal_marker (fixture, journal);
    assert_check_fails (fixture, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        "does not exist but");

    g_assert_cmpint (g_remove (journal_path), ==, 0);
    write_object_marker (fixture, object);
    assert_check_fails (fixture, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        "does not exist but");
    g_assert_false (g_file_test (journal_path, G_FILE_TEST_EXISTS));
    g_assert_true (g_file_test (object_path, G_FILE_TEST_EXISTS));
}

static void
test_check_rejects_malformed_marker (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *object = marker_text ("object-store", STORAGE_ID_A);

    (void)user_data;

    make_roots (fixture);
    write_journal_marker (fixture, "not a key file\n");
    write_object_marker (fixture, object);
    assert_check_fails (fixture, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "is invalid");
}

static void
test_check_rejects_wrong_role (StorageFixture *fixture, gconstpointer user_data)
{
    g_autofree char *journal = marker_text ("journal", STORAGE_ID_A);

    (void)user_data;

    make_roots (fixture);
    write_journal_marker (fixture, journal);
    write_object_marker (fixture, journal);
    assert_check_fails (fixture, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "is invalid");
}

static void
test_check_rejects_bad_storage_ids (StorageFixture *fixture,
    gconstpointer user_data)
{
    const char *bad_ids[] = {
        "not-a-uuid",
        "1b4e28ba-2fa1-4d2e-883f-0016d3cca427",
        "00000000-0000-0000-0000-000000000000",
        "01920000-0000-7000-8000-0000000000011",
    };

    (void)user_data;

    make_roots (fixture);
    for (gsize i = 0; i < G_N_ELEMENTS (bad_ids); i++) {
        g_autofree char *journal = marker_text ("journal", bad_ids[i]);
        g_autofree char *object = marker_text ("object-store", bad_ids[i]);

        write_journal_marker (fixture, journal);
        write_object_marker (fixture, object);
        assert_check_fails (fixture, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "is invalid");
    }
}

static void
test_check_rejects_unknown_format (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *object = marker_text ("object-store", STORAGE_ID_A);

    (void)user_data;

    make_roots (fixture);
    write_journal_marker (fixture, "[WyreBox Storage]\nformat=2\n"
        "role=journal\nstorage_id=" STORAGE_ID_A "\n");
    write_object_marker (fixture, object);
    assert_check_fails (fixture, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
        "unsupported format 2");
}

static void
test_check_rejects_oversized_marker (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *journal = marker_text ("journal", STORAGE_ID_A);
    g_autofree char *padding = g_strnfill (8192, '#');
    g_autofree char *object = g_strconcat (padding, "\n",
            "[WyreBox Storage]\nformat=1\nrole=object-store\nstorage_id="
            STORAGE_ID_A "\n", NULL);

    (void)user_data;

    make_roots (fixture);
    write_journal_marker (fixture, journal);
    write_object_marker (fixture, object);
    assert_check_fails (fixture, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "is invalid");
}

static void
test_check_rejects_mismatched_storage_ids (StorageFixture *fixture,
    gconstpointer user_data)
{
    (void)user_data;

    write_valid_markers (fixture, STORAGE_ID_A, STORAGE_ID_B);
    assert_check_fails (fixture, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "storage markers do not match");
}

static void
test_check_reports_unreadable_marker (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *object = marker_text ("object-store", STORAGE_ID_A);
    g_autofree char *journal_path = journal_marker_path (fixture);
    g_autoptr (GError) error = NULL;

    (void)user_data;

    make_roots (fixture);
    g_assert_cmpint (g_mkdir (journal_path, 0700), ==, 0);
    write_object_marker (fixture, object);
    g_assert_false (wyrebox_daemon_storage_check_initialized
            (fixture->journal_root, fixture->object_root, NULL, &error));
    g_assert_nonnull (error);
    g_assert_true (error->domain == G_IO_ERROR);
    g_assert_cmpint (error->code, !=, G_IO_ERROR_NOT_FOUND);
    g_assert_cmpint (error->code, !=, G_IO_ERROR_INVALID_DATA);
    g_assert_cmpint (error->code, !=, G_IO_ERROR_NOT_SUPPORTED);
    g_assert_nonnull (strstr (error->message, "failed to read storage marker"));
}

static void
test_check_reports_permission_denied_marker (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *journal_path = journal_marker_path (fixture);

    (void)user_data;

    if (geteuid () == 0) {
        g_test_skip ("root bypasses file permissions");
        return;
    }

    write_valid_markers (fixture, STORAGE_ID_A, STORAGE_ID_A);
    g_assert_cmpint (g_chmod (journal_path, 0), ==, 0);
    assert_check_fails (fixture, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
        "failed to read storage marker");
}

static void
test_check_returns_storage_id (StorageFixture *fixture,
    gconstpointer user_data)
{
    (void)user_data;

    write_valid_markers (fixture, STORAGE_ID_A, STORAGE_ID_A);
    assert_checks_with_id (fixture, STORAGE_ID_A);
}

static void
test_initialize_creates_roots_and_paired_markers (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *storage_id = NULL;
    g_autofree char *journal_path = journal_marker_path (fixture);
    g_autofree char *object_path = object_marker_path (fixture);
    g_autofree char *journal_id = NULL;
    g_autofree char *object_id = NULL;

    (void)user_data;

    storage_id = initialize_ok (fixture, FALSE);
    g_assert_cmpuint (strlen (storage_id), ==, 36);
    g_assert_cmpint (storage_id[14], ==, '7');
    g_assert_true (objects_dir_exists (fixture));
    journal_id = marker_storage_id (journal_path);
    object_id = marker_storage_id (object_path);
    g_assert_cmpstr (journal_id, ==, storage_id);
    g_assert_cmpstr (object_id, ==, storage_id);
    assert_checks_with_id (fixture, storage_id);
}

static void
test_initialize_is_idempotent (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *first = NULL;
    g_autofree char *second = NULL;
    g_autofree char *journal_path = journal_marker_path (fixture);
    g_autofree char *object_path = object_marker_path (fixture);
    g_autofree char *journal_before = NULL;
    g_autofree char *object_before = NULL;
    g_autofree char *journal_after = NULL;
    g_autofree char *object_after = NULL;

    (void)user_data;

    first = initialize_ok (fixture, FALSE);
    journal_before = read_file (journal_path);
    object_before = read_file (object_path);
    second = initialize_ok (fixture, TRUE);
    journal_after = read_file (journal_path);
    object_after = read_file (object_path);
    g_assert_cmpstr (second, ==, first);
    g_assert_cmpstr (journal_after, ==, journal_before);
    g_assert_cmpstr (object_after, ==, object_before);
}

static void
test_initialize_same_directory_roots (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *storage_id = NULL;

    (void)user_data;

    g_free (fixture->object_root);
    fixture->object_root = g_strdup (fixture->journal_root);
    storage_id = initialize_ok (fixture, FALSE);
    assert_checks_with_id (fixture, storage_id);
}

static void
test_initialize_treats_empty_segment_as_empty (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *storage_id = NULL;

    (void)user_data;

    write_journal_data (fixture, 0);
    storage_id = initialize_ok (fixture, FALSE);
    assert_checks_with_id (fixture, storage_id);
}

static void
test_initialize_adopts_populated_unmarked_storage (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *storage_id = NULL;
    g_autofree char *segment = segment_path (fixture);
    g_autofree char *object = object_data_path (fixture);
    g_autofree char *segment_bytes = NULL;
    g_autofree char *object_bytes = NULL;

    (void)user_data;

    write_journal_data (fixture, -1);
    write_object_data (fixture);
    storage_id = initialize_ok (fixture, FALSE);
    segment_bytes = read_file (segment);
    object_bytes = read_file (object);
    g_assert_cmpstr (segment_bytes, ==, "WYREJNL1-journal-bytes");
    g_assert_cmpstr (object_bytes, ==, "raw object bytes");
    assert_checks_with_id (fixture, storage_id);
}

static void
test_initialize_refuses_journal_data_without_objects (StorageFixture *fixture,
    gconstpointer user_data)
{
    (void)user_data;

    write_journal_data (fixture, -1);
    assert_initialize_fails (fixture, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        "check that the object store is mounted");
    g_assert_false (objects_dir_exists (fixture));
    g_assert_false (g_file_test (fixture->object_root, G_FILE_TEST_EXISTS));
    g_assert_false (any_marker_exists (fixture));
}

static void
test_initialize_refuses_torn_journal_without_objects (StorageFixture *fixture,
    gconstpointer user_data)
{
    (void)user_data;

    write_journal_data (fixture, 1);
    assert_initialize_fails (fixture, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        "check that the object store is mounted");
    g_assert_false (objects_dir_exists (fixture));
    g_assert_false (any_marker_exists (fixture));
}

static void
test_initialize_refuses_objects_without_journal_data (StorageFixture *fixture,
    gconstpointer user_data)
{
    (void)user_data;

    write_object_data (fixture);
    assert_initialize_fails (fixture, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        "check that the journal is mounted");
    g_assert_false (g_file_test (fixture->journal_root, G_FILE_TEST_EXISTS));
    g_assert_false (any_marker_exists (fixture));
}

static void
test_initialize_completes_lone_marker_when_both_empty (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *journal = marker_text ("journal", STORAGE_ID_A);
    g_autofree char *storage_id = NULL;

    (void)user_data;

    g_assert_cmpint (g_mkdir_with_parents (fixture->journal_root, 0700), ==,
        0);
    write_journal_marker (fixture, journal);
    storage_id = initialize_ok (fixture, FALSE);
    g_assert_cmpstr (storage_id, ==, STORAGE_ID_A);
    assert_checks_with_id (fixture, STORAGE_ID_A);
}

static void
test_initialize_completes_lone_marker_when_both_populated (StorageFixture
    *fixture, gconstpointer user_data)
{
    g_autofree char *object = marker_text ("object-store", STORAGE_ID_B);
    g_autofree char *storage_id = NULL;

    (void)user_data;

    write_journal_data (fixture, -1);
    write_object_data (fixture);
    write_object_marker (fixture, object);
    storage_id = initialize_ok (fixture, FALSE);
    g_assert_cmpstr (storage_id, ==, STORAGE_ID_B);
    assert_checks_with_id (fixture, STORAGE_ID_B);
}

static void
test_initialize_refuses_lone_marker_with_one_sided_data (StorageFixture
    *fixture, gconstpointer user_data)
{
    g_autofree char *journal = marker_text ("journal", STORAGE_ID_A);
    g_autofree char *object_path = object_marker_path (fixture);

    (void)user_data;

    write_journal_data (fixture, -1);
    write_journal_marker (fixture, journal);
    assert_initialize_fails (fixture, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        "check that the object store is mounted");
    g_assert_false (objects_dir_exists (fixture));
    g_assert_false (g_file_test (object_path, G_FILE_TEST_EXISTS));
}

static void
test_initialize_refuses_mismatched_markers (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *journal_path = journal_marker_path (fixture);
    g_autofree char *object_path = object_marker_path (fixture);
    g_autofree char *journal_before = NULL;
    g_autofree char *object_before = NULL;
    g_autofree char *journal_after = NULL;
    g_autofree char *object_after = NULL;

    (void)user_data;

    write_valid_markers (fixture, STORAGE_ID_A, STORAGE_ID_B);
    journal_before = read_file (journal_path);
    object_before = read_file (object_path);
    assert_initialize_fails (fixture, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "storage markers do not match");
    journal_after = read_file (journal_path);
    object_after = read_file (object_path);
    g_assert_cmpstr (journal_after, ==, journal_before);
    g_assert_cmpstr (object_after, ==, object_before);
    g_assert_false (objects_dir_exists (fixture));
}

static void
test_initialize_refuses_invalid_marker (StorageFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *object_path = object_marker_path (fixture);

    (void)user_data;

    make_roots (fixture);
    write_journal_marker (fixture, "garbage\n");
    assert_initialize_fails (fixture, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "is invalid");
    g_assert_false (g_file_test (object_path, G_FILE_TEST_EXISTS));
    g_assert_false (objects_dir_exists (fixture));
}

#define ADD_STORAGE_TEST(path, func) \
        g_test_add ("/daemon-api/storage/" path, StorageFixture, NULL, \
            storage_fixture_set_up, func, storage_fixture_tear_down)

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    ADD_STORAGE_TEST ("check/fails-when-roots-are-missing",
        test_check_fails_when_roots_are_missing);
    ADD_STORAGE_TEST ("check/fails-when-roots-are-empty",
        test_check_fails_when_roots_are_empty);
    ADD_STORAGE_TEST ("check/fails-when-one-marker-is-missing",
        test_check_fails_when_one_marker_is_missing);
    ADD_STORAGE_TEST ("check/rejects-malformed-marker",
        test_check_rejects_malformed_marker);
    ADD_STORAGE_TEST ("check/rejects-wrong-role",
        test_check_rejects_wrong_role);
    ADD_STORAGE_TEST ("check/rejects-bad-storage-ids",
        test_check_rejects_bad_storage_ids);
    ADD_STORAGE_TEST ("check/rejects-unknown-format",
        test_check_rejects_unknown_format);
    ADD_STORAGE_TEST ("check/rejects-oversized-marker",
        test_check_rejects_oversized_marker);
    ADD_STORAGE_TEST ("check/rejects-mismatched-storage-ids",
        test_check_rejects_mismatched_storage_ids);
    ADD_STORAGE_TEST ("check/reports-unreadable-marker",
        test_check_reports_unreadable_marker);
    ADD_STORAGE_TEST ("check/reports-permission-denied-marker",
        test_check_reports_permission_denied_marker);
    ADD_STORAGE_TEST ("check/returns-storage-id",
        test_check_returns_storage_id);
    ADD_STORAGE_TEST ("initialize/creates-roots-and-paired-markers",
        test_initialize_creates_roots_and_paired_markers);
    ADD_STORAGE_TEST ("initialize/is-idempotent",
        test_initialize_is_idempotent);
    ADD_STORAGE_TEST ("initialize/same-directory-roots",
        test_initialize_same_directory_roots);
    ADD_STORAGE_TEST ("initialize/treats-empty-segment-as-empty",
        test_initialize_treats_empty_segment_as_empty);
    ADD_STORAGE_TEST ("initialize/adopts-populated-unmarked-storage",
        test_initialize_adopts_populated_unmarked_storage);
    ADD_STORAGE_TEST ("initialize/refuses-journal-data-without-objects",
        test_initialize_refuses_journal_data_without_objects);
    ADD_STORAGE_TEST ("initialize/refuses-torn-journal-without-objects",
        test_initialize_refuses_torn_journal_without_objects);
    ADD_STORAGE_TEST ("initialize/refuses-objects-without-journal-data",
        test_initialize_refuses_objects_without_journal_data);
    ADD_STORAGE_TEST ("initialize/completes-lone-marker-when-both-empty",
        test_initialize_completes_lone_marker_when_both_empty);
    ADD_STORAGE_TEST ("initialize/completes-lone-marker-when-both-populated",
        test_initialize_completes_lone_marker_when_both_populated);
    ADD_STORAGE_TEST ("initialize/refuses-lone-marker-with-one-sided-data",
        test_initialize_refuses_lone_marker_with_one_sided_data);
    ADD_STORAGE_TEST ("initialize/refuses-mismatched-markers",
        test_initialize_refuses_mismatched_markers);
    ADD_STORAGE_TEST ("initialize/refuses-invalid-marker",
        test_initialize_refuses_invalid_marker);

    return g_test_run ();
}
