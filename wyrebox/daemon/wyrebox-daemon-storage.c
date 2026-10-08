#include "wyrebox-daemon-storage.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-local-object-store.h"

#include <chronoid/uuidv7.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#define STORAGE_MARKER_GROUP "WyreBox Storage"
#define STORAGE_MARKER_FORMAT 1
#define STORAGE_MARKER_MAX_SIZE 4096
#define STORAGE_ROLE_JOURNAL "journal"
#define STORAGE_ROLE_OBJECT_STORE "object-store"

typedef struct
{
    char *path;
    char *storage_id;
} StorageMarker;

static void
storage_marker_clear (StorageMarker *marker)
{
    g_clear_pointer (&marker->path, g_free);
    g_clear_pointer (&marker->storage_id, g_free);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (StorageMarker, storage_marker_clear)

static void
set_errno_error (GError **error, int saved_errno, const char *action,
    const char *path)
{
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
        "failed to %s %s: %s", action, path, g_strerror (saved_errno));
}

static gboolean
parse_storage_id (const char *text, char **out_storage_id,
    const char **out_reason)
{
    chronoid_uuidv7_t id = { 0 };
    char canonical[CHRONOID_UUIDV7_STRING_LEN + 1] = { 0 };

    if (strlen (text) != CHRONOID_UUIDV7_STRING_LEN ||
        chronoid_uuidv7_parse (&id, text, CHRONOID_UUIDV7_STRING_LEN) !=
        CHRONOID_UUIDV7_OK) {
        *out_reason = "storage_id is not a UUID";
        return FALSE;
    }

    if (chronoid_uuidv7_is_nil (&id) || chronoid_uuidv7_version (&id) != 7 ||
        chronoid_uuidv7_variant (&id) != 2) {
        *out_reason = "storage_id is not a UUIDv7";
        return FALSE;
    }

    chronoid_uuidv7_format (&id, canonical);
    *out_storage_id = g_strdup (canonical);
    return TRUE;
}

static gboolean
read_marker_bytes (const char *path, GBytes **out_bytes, GError **error)
{
    g_autofree char *buffer = g_malloc (STORAGE_MARKER_MAX_SIZE + 1);
    gsize length = 0;
    struct stat st = { 0 };
    int fd = -1;

    *out_bytes = NULL;
    fd = g_open (path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        int saved_errno = errno;

        if (saved_errno == ENOENT)
            return TRUE;

        set_errno_error (error, saved_errno,
            "read storage marker", path);
        return FALSE;
    }

    if (fstat (fd, &st) != 0) {
        int saved_errno = errno;

        (void)close (fd);
        set_errno_error (error, saved_errno,
            "read storage marker", path);
        return FALSE;
    }

    if (!S_ISREG (st.st_mode)) {
        (void)close (fd);
        g_set_error (error, G_IO_ERROR,
            S_ISDIR (st.st_mode) ? G_IO_ERROR_IS_DIRECTORY :
            G_IO_ERROR_NOT_REGULAR_FILE,
            "failed to read storage marker %s: not a regular file", path);
        return FALSE;
    }

    while (length <= STORAGE_MARKER_MAX_SIZE) {
        ssize_t n = read (fd, buffer + length,
                STORAGE_MARKER_MAX_SIZE + 1 - length);

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            int saved_errno = errno;

            (void)close (fd);
            set_errno_error (error, saved_errno,
                "read storage marker", path);
            return FALSE;
        }
        if (n == 0)
            break;
        length += (gsize)n;
    }
    (void)close (fd);

    if (length > STORAGE_MARKER_MAX_SIZE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "storage marker %s is invalid: larger than %d bytes", path,
            STORAGE_MARKER_MAX_SIZE);
        return FALSE;
    }

    *out_bytes = g_bytes_new (buffer, length);
    return TRUE;
}

/*
 * Reads the marker @name under @root_dir. A missing marker is not an error:
 * it returns TRUE with marker->storage_id left NULL.
 */
static gboolean
read_marker (const char *root_dir, const char *name, const char *role,
    StorageMarker *marker, GError **error)
{
    g_autoptr (GBytes) bytes = NULL;
    g_autoptr (GKeyFile) key_file = g_key_file_new ();
    g_autoptr (GError) local_error = NULL;
    g_autofree char *found_role = NULL;
    g_autofree char *storage_id = NULL;
    const char *reason = NULL;
    gsize length = 0;
    const char *data = NULL;
    gint format = 0;

    marker->path = g_build_filename (root_dir, name, NULL);
    if (!read_marker_bytes (marker->path, &bytes, error))
        return FALSE;
    if (bytes == NULL)
        return TRUE;

    data = g_bytes_get_data (bytes, &length);
    if (!g_key_file_load_from_data (key_file, data, length, G_KEY_FILE_NONE,
        &local_error)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "storage marker %s is invalid: %s", marker->path,
            local_error->message);
        return FALSE;
    }

    format = g_key_file_get_integer (key_file, STORAGE_MARKER_GROUP, "format",
            &local_error);
    if (local_error != NULL) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "storage marker %s is invalid: %s", marker->path,
            local_error->message);
        return FALSE;
    }
    if (format != STORAGE_MARKER_FORMAT) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "storage marker %s has unsupported format %d", marker->path,
            format);
        return FALSE;
    }

    found_role = g_key_file_get_string (key_file, STORAGE_MARKER_GROUP, "role",
            &local_error);
    if (local_error != NULL || g_strcmp0 (found_role, role) != 0) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "storage marker %s is invalid: role is not %s", marker->path,
            role);
        return FALSE;
    }

    storage_id = g_key_file_get_string (key_file, STORAGE_MARKER_GROUP,
            "storage_id", &local_error);
    if (local_error != NULL) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "storage marker %s is invalid: %s", marker->path,
            local_error->message);
        return FALSE;
    }
    if (!parse_storage_id (storage_id, &marker->storage_id, &reason)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "storage marker %s is invalid: %s", marker->path, reason);
        return FALSE;
    }

    return TRUE;
}

static gboolean
read_markers (const char *journal_root_dir, const char *object_root_dir,
    StorageMarker *journal, StorageMarker *object, GError **error)
{
    if (!read_marker (journal_root_dir, WYREBOX_DAEMON_STORAGE_JOURNAL_MARKER,
        STORAGE_ROLE_JOURNAL, journal, error))
        return FALSE;

    return read_marker (object_root_dir,
               WYREBOX_DAEMON_STORAGE_OBJECT_STORE_MARKER,
               STORAGE_ROLE_OBJECT_STORE, object, error);
}

static gboolean
check_markers_match (const StorageMarker *journal, const StorageMarker *object,
    GError **error)
{
    if (g_strcmp0 (journal->storage_id, object->storage_id) == 0)
        return TRUE;

    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "storage markers do not match: journal %s has storage ID %s but "
        "object store %s has storage ID %s", journal->path,
        journal->storage_id, object->path, object->storage_id);
    return FALSE;
}

gboolean
wyrebox_daemon_storage_check_initialized (const char *journal_root_dir,
    const char *object_root_dir, char **out_storage_id, GError **error)
{
    g_auto (StorageMarker) journal = { 0 };
    g_auto (StorageMarker) object = { 0 };

    g_return_val_if_fail (journal_root_dir != NULL, FALSE);
    g_return_val_if_fail (object_root_dir != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (out_storage_id != NULL)
        *out_storage_id = NULL;

    if (!read_markers (journal_root_dir, object_root_dir, &journal, &object,
        error))
        return FALSE;

    if (journal.storage_id == NULL && object.storage_id == NULL) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "storage is not initialized: %s and %s do not exist; check that "
            "the storage volume is mounted, or run wyreboxd "
            "--initialize-storage once on a new installation", journal.path,
            object.path);
        return FALSE;
    }

    if (journal.storage_id == NULL || object.storage_id == NULL) {
        const StorageMarker *missing =
            journal.storage_id == NULL ? &journal : &object;
        const StorageMarker *present =
            journal.storage_id == NULL ? &object : &journal;
        const char *missing_root =
            journal.storage_id == NULL ? journal_root_dir : object_root_dir;

        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "storage marker %s does not exist but %s does; check that the "
            "volume holding %s is mounted, or run wyreboxd "
            "--initialize-storage again if it was interrupted", missing->path,
            present->path, missing_root);
        return FALSE;
    }

    if (!check_markers_match (&journal, &object, error))
        return FALSE;

    if (out_storage_id != NULL)
        *out_storage_id = g_steal_pointer (&journal.storage_id);
    return TRUE;
}

static gboolean
journal_holds_data (const char *journal_root_dir, gboolean *out_holds_data,
    GError **error)
{
    g_autofree char *segment = g_build_filename (journal_root_dir,
            WYREBOX_JOURNAL_SEGMENT_NAME, NULL);
    GStatBuf st = { 0 };

    *out_holds_data = FALSE;
    if (g_stat (segment, &st) != 0) {
        int saved_errno = errno;

        if (saved_errno == ENOENT)
            return TRUE;

        set_errno_error (error, saved_errno,
            "inspect journal segment", segment);
        return FALSE;
    }

    *out_holds_data = st.st_size > 0;
    return TRUE;
}

static gboolean
object_store_holds_data (const char *object_root_dir,
    gboolean *out_holds_data, GError **error)
{
    g_autofree char *objects_dir = g_build_filename (object_root_dir,
            "objects", "sha256", NULL);
    g_autoptr (GDir) dir = NULL;
    g_autoptr (GError) local_error = NULL;

    *out_holds_data = FALSE;
    dir = g_dir_open (objects_dir, 0, &local_error);
    if (dir == NULL) {
        if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            return TRUE;

        g_set_error (error, G_IO_ERROR,
            g_io_error_from_file_error (local_error->code),
            "failed to inspect object store root %s: %s", objects_dir,
            local_error->message);
        return FALSE;
    }

    *out_holds_data = g_dir_read_name (dir) != NULL;
    return TRUE;
}

static gboolean
fsync_directory (const char *path, GError **error)
{
    int fd = g_open (path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);

    if (fd < 0 || fsync (fd) != 0) {
        int saved_errno = errno;

        if (fd >= 0)
            (void)close (fd);
        set_errno_error (error, saved_errno,
            "sync directory", path);
        return FALSE;
    }

    (void)close (fd);
    return TRUE;
}

static gboolean
create_journal_root (const char *journal_root_dir, GError **error)
{
    g_autofree char *parent = g_path_get_dirname (journal_root_dir);

    if (g_mkdir_with_parents (journal_root_dir, 0700) != 0) {
        set_errno_error (error, errno, "create journal root",
            journal_root_dir);
        return FALSE;
    }

    return fsync_directory (parent, error) &&
           fsync_directory (journal_root_dir, error);
}

static gboolean
create_object_root (const char *object_root_dir, GError **error)
{
    g_autoptr (WyreboxLocalObjectStore) store =
        wyrebox_local_object_store_new (object_root_dir, error);

    return store != NULL;
}

static gboolean
write_marker (const char *root_dir, const char *path, const char *role,
    const char *storage_id, GError **error)
{
    g_autofree char *contents = g_strdup_printf ("[" STORAGE_MARKER_GROUP "]\n"
            "format=%d\nrole=%s\nstorage_id=%s\n", STORAGE_MARKER_FORMAT,
            role, storage_id);
    g_autoptr (GError) local_error = NULL;

    if (!g_file_set_contents_full (path, contents, -1,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0640,
        &local_error)) {
        g_set_error (error, G_IO_ERROR,
            g_io_error_from_file_error (local_error->code),
            "failed to write storage marker %s: %s", path,
            local_error->message);
        return FALSE;
    }

    return fsync_directory (root_dir, error);
}

static char *
generate_storage_id (GError **error)
{
    chronoid_uuidv7_t id = { 0 };
    char text[CHRONOID_UUIDV7_STRING_LEN + 1] = { 0 };

    if (chronoid_uuidv7_new (&id) != CHRONOID_UUIDV7_OK) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "failed to generate storage ID: no random source available");
        return NULL;
    }

    chronoid_uuidv7_format (&id, text);
    return g_strdup (text);
}

gboolean
wyrebox_daemon_storage_initialize (const char *journal_root_dir,
    const char *object_root_dir, gboolean *out_already_initialized,
    char **out_storage_id, GError **error)
{
    g_auto (StorageMarker) journal = { 0 };
    g_auto (StorageMarker) object = { 0 };
    g_autofree char *storage_id = NULL;
    gboolean journal_data = FALSE;
    gboolean object_data = FALSE;

    g_return_val_if_fail (journal_root_dir != NULL, FALSE);
    g_return_val_if_fail (object_root_dir != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (out_already_initialized != NULL)
        *out_already_initialized = FALSE;
    if (out_storage_id != NULL)
        *out_storage_id = NULL;

    if (!read_markers (journal_root_dir, object_root_dir, &journal, &object,
        error))
        return FALSE;

    if (journal.storage_id != NULL && object.storage_id != NULL) {
        if (!check_markers_match (&journal, &object, error))
            return FALSE;

        if (out_already_initialized != NULL)
            *out_already_initialized = TRUE;
        if (out_storage_id != NULL)
            *out_storage_id = g_steal_pointer (&journal.storage_id);
        return TRUE;
    }

    /* Initialization writes the journal marker first, so an interrupted run
     * never leaves the object store marker alone. */
    if (journal.storage_id == NULL && object.storage_id != NULL) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "cannot initialize storage: %s exists but %s does not; check "
            "that the journal is mounted", object.path, journal.path);
        return FALSE;
    }

    if (!journal_holds_data (journal_root_dir, &journal_data, error) ||
        !object_store_holds_data (object_root_dir, &object_data, error))
        return FALSE;

    if (journal_data && !object_data) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "cannot initialize storage: journal %s is not empty but object "
            "store %s has no objects; check that the object store is mounted",
            journal_root_dir, object_root_dir);
        return FALSE;
    }

    if (object_data && !journal_data) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "cannot initialize storage: object store %s has objects but "
            "journal %s is empty; check that the journal is mounted",
            object_root_dir, journal_root_dir);
        return FALSE;
    }

    if (journal.storage_id != NULL)
        storage_id = g_strdup (journal.storage_id);
    else
        storage_id = generate_storage_id (error);
    if (storage_id == NULL)
        return FALSE;

    if (!create_journal_root (journal_root_dir, error) ||
        !create_object_root (object_root_dir, error))
        return FALSE;

    if (journal.storage_id == NULL &&
        !write_marker (journal_root_dir, journal.path, STORAGE_ROLE_JOURNAL,
        storage_id, error))
        return FALSE;

    if (object.storage_id == NULL &&
        !write_marker (object_root_dir, object.path,
        STORAGE_ROLE_OBJECT_STORE, storage_id, error))
        return FALSE;

    if (out_storage_id != NULL)
        *out_storage_id = g_steal_pointer (&storage_id);
    return TRUE;
}
