#include "wyrebox-local-object-store.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

struct _WyreboxLocalObjectStore
{
  GObject parent_instance;
  char *root_dir;
  gboolean writable;
};

G_DEFINE_TYPE (WyreboxLocalObjectStore,
    wyrebox_local_object_store, G_TYPE_OBJECT);

static void
wyrebox_local_object_store_finalize (GObject *object)
{
  WyreboxLocalObjectStore *self = WYREBOX_LOCAL_OBJECT_STORE (object);

  g_clear_pointer (&self->root_dir, g_free);

  G_OBJECT_CLASS (wyrebox_local_object_store_parent_class)->finalize (object);
}

static void
wyrebox_local_object_store_class_init (WyreboxLocalObjectStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_local_object_store_finalize;
}

static void
wyrebox_local_object_store_init (WyreboxLocalObjectStore *self)
{
}

static gboolean
validate_object_key (const char *object_key, const char **out_hex,
    GError **error)
{
  const char *hex = NULL;

  if (object_key == NULL || !g_str_has_prefix (object_key, "sha256:")) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid object key");
    return FALSE;
  }

  hex = object_key + strlen ("sha256:");
  if (strlen (hex) != 64) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "invalid sha256 object key length");
    return FALSE;
  }

  for (size_t index = 0; index < 64; index++) {
    if (!g_ascii_isxdigit (hex[index]) || g_ascii_isupper (hex[index])) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT, "invalid sha256 object key hex");
      return FALSE;
    }
  }

  if (out_hex != NULL)
    *out_hex = hex;

  return TRUE;
}

static char *
build_object_path (WyreboxLocalObjectStore *self, const char *hex)
{
  g_autofree char *prefix = g_strndup (hex, 2);
  g_autofree char *filename = g_strdup_printf ("%s.eml", hex);

  return g_build_filename (self->root_dir,
      "objects", "sha256", prefix, filename, NULL);
}

static char *
checksum_bytes (GBytes *bytes)
{
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (bytes, &size);
  g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

  g_checksum_update (checksum, data, size);

  return g_strdup (g_checksum_get_string (checksum));
}

static char *
checksum_data (const guint8 *data, gsize size)
{
  g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

  g_checksum_update (checksum, data, size);

  return g_strdup (g_checksum_get_string (checksum));
}

static gboolean
verify_object_contents (const char *path,
    const char *expected_hex, const guint8 *data, gsize size, GError **error)
{
  g_autofree char *actual_hex = checksum_data (data, size);

  if (g_strcmp0 (actual_hex, expected_hex) == 0)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "object %s failed SHA-256 verification: expected %s, got %s",
      path, expected_hex, actual_hex);
  return FALSE;
}

static gboolean
verify_object_file (const char *path, const char *expected_hex, GError **error)
{
  g_autofree char *contents = NULL;
  gsize length = 0;

  if (!g_file_get_contents (path, &contents, &length, error))
    return FALSE;

  return verify_object_contents (path,
      expected_hex, (const guint8 *) contents, length, error);
}

static gboolean
write_all (int fd, const guint8 *data, gsize size, GError **error)
{
  while (size > 0) {
    ssize_t written = write (fd, data, size);

    if (written < 0) {
      int saved_errno = errno;

      if (saved_errno == EINTR)
        continue;

      g_set_error (error,
          G_IO_ERROR,
          g_io_error_from_errno (saved_errno),
          "failed to write object: %s", g_strerror (saved_errno));
      return FALSE;
    }

    data += written;
    size -= (gsize) written;
  }

  return TRUE;
}

static gboolean
fsync_directory_path (const char *path, GError **error)
{
  g_autofd int fd = -1;

  fd = open (path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to open directory %s for fsync: %s",
        path, g_strerror (saved_errno));
    return FALSE;
  }

  if (fsync (fd) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to fsync directory %s: %s", path, g_strerror (saved_errno));
    return FALSE;
  }

  return TRUE;
}

static void
fsync_directory_path_best_effort (const char *path)
{
  g_autoptr (GError) ignored_error = NULL;

  (void) fsync_directory_path (path, &ignored_error);
}

static gboolean
fsync_object_root_setup (const char *root_dir,
    const char *objects_dir, GError **error)
{
  g_autofree char *objects_parent_dir = g_path_get_dirname (objects_dir);
  g_autofree char *root_parent_dir = g_path_get_dirname (root_dir);

  if (!fsync_directory_path (root_parent_dir, error))
    return FALSE;

  if (!fsync_directory_path (root_dir, error))
    return FALSE;

  if (!fsync_directory_path (objects_parent_dir, error))
    return FALSE;

  if (!fsync_directory_path (objects_dir, error))
    return FALSE;

  return TRUE;
}

static WyreboxLocalObjectStore *
local_object_store_alloc (const char *root_dir, gboolean writable)
{
  WyreboxLocalObjectStore *self = NULL;

  self = g_object_new (WYREBOX_TYPE_LOCAL_OBJECT_STORE, NULL);
  self->root_dir = g_strdup (root_dir);
  self->writable = writable;

  return self;
}

WyreboxLocalObjectStore *
wyrebox_local_object_store_new (const char *root_dir, GError **error)
{
  g_autoptr (WyreboxLocalObjectStore) self = NULL;
  g_autofree char *objects_dir = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (root_dir == NULL || *root_dir == '\0') {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "root directory is required");
    return NULL;
  }

  objects_dir = g_build_filename (root_dir, "objects", "sha256", NULL);
  if (g_mkdir_with_parents (objects_dir, 0700) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to create object store root %s: %s",
        objects_dir, g_strerror (saved_errno));
    return NULL;
  }

  if (!fsync_object_root_setup (root_dir, objects_dir, error))
    return NULL;

  self = local_object_store_alloc (root_dir, TRUE);

  return g_steal_pointer (&self);
}

WyreboxLocalObjectStore *
wyrebox_local_object_store_open_existing (const char *root_dir, GError **error)
{
  g_autoptr (WyreboxLocalObjectStore) self = NULL;
  g_autofree char *objects_dir = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (root_dir == NULL || *root_dir == '\0') {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "root directory is required");
    return NULL;
  }

  objects_dir = g_build_filename (root_dir, "objects", "sha256", NULL);
  if (!g_file_test (objects_dir, G_FILE_TEST_IS_DIR)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "object store root %s does not exist", objects_dir);
    return NULL;
  }

  self = local_object_store_alloc (root_dir, FALSE);

  return g_steal_pointer (&self);
}

gboolean
wyrebox_local_object_store_put_bytes (WyreboxLocalObjectStore *self,
    GBytes *bytes, char **out_object_key, GError **error)
{
  g_autofree char *hex = NULL;
  g_autofree char *path = NULL;
  g_autofree char *parent_dir = NULL;
  g_autofree char *hash_root_dir = NULL;
  g_autofree char *temp_path = NULL;
  g_autofd int fd = -1;
  gsize size = 0;
  const guint8 *data = NULL;

  g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (self), FALSE);
  g_return_val_if_fail (bytes != NULL, FALSE);
  g_return_val_if_fail (out_object_key != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!self->writable) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED, "object store was opened read-only");
    return FALSE;
  }

  *out_object_key = NULL;
  hex = checksum_bytes (bytes);
  path = build_object_path (self, hex);
  parent_dir = g_path_get_dirname (path);
  hash_root_dir = g_path_get_dirname (parent_dir);

  if (g_mkdir_with_parents (parent_dir, 0700) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to create object directory %s: %s",
        parent_dir, g_strerror (saved_errno));
    return FALSE;
  }

  if (!fsync_directory_path (hash_root_dir, error))
    return FALSE;

  if (g_file_test (path, G_FILE_TEST_EXISTS)) {
    if (!verify_object_file (path, hex, error))
      return FALSE;

    if (!fsync_directory_path (parent_dir, error))
      return FALSE;

    *out_object_key = g_strdup_printf ("sha256:%s", hex);
    return TRUE;
  }

  temp_path = g_build_filename (parent_dir, ".tmp-object-XXXXXX", NULL);
  fd = g_mkstemp_full (temp_path, O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to create temporary object %s: %s",
        temp_path, g_strerror (saved_errno));
    return FALSE;
  }

  data = g_bytes_get_data (bytes, &size);
  if (!write_all (fd, data, size, error)) {
    (void) g_unlink (temp_path);
    fsync_directory_path_best_effort (parent_dir);
    return FALSE;
  }

  if (fsync (fd) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to fsync temporary object %s: %s",
        temp_path, g_strerror (saved_errno));
    (void) g_unlink (temp_path);
    fsync_directory_path_best_effort (parent_dir);
    return FALSE;
  }

  if (close (g_steal_fd (&fd)) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to close temporary object %s: %s",
        temp_path, g_strerror (saved_errno));
    (void) g_unlink (temp_path);
    fsync_directory_path_best_effort (parent_dir);
    return FALSE;
  }

  if (link (temp_path, path) != 0) {
    int saved_errno = errno;

    (void) g_unlink (temp_path);
    fsync_directory_path_best_effort (parent_dir);

    if (saved_errno == EEXIST) {
      if (!verify_object_file (path, hex, error))
        return FALSE;

      if (!fsync_directory_path (parent_dir, error))
        return FALSE;

      *out_object_key = g_strdup_printf ("sha256:%s", hex);
      return TRUE;
    }

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to install object %s: %s", path, g_strerror (saved_errno));
    return FALSE;
  }

  (void) g_unlink (temp_path);
  if (!fsync_directory_path (parent_dir, error))
    return FALSE;

  *out_object_key = g_strdup_printf ("sha256:%s", hex);
  return TRUE;
}

GBytes *
wyrebox_local_object_store_get_bytes (WyreboxLocalObjectStore *self,
    const char *object_key, GError **error)
{
  const char *hex = NULL;
  g_autofree char *path = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;

  g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (self), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!validate_object_key (object_key, &hex, error))
    return NULL;

  path = build_object_path (self, hex);
  if (!g_file_get_contents (path, &contents, &length, error))
    return NULL;

  if (!verify_object_contents (path,
          hex, (const guint8 *) contents, length, error))
    return NULL;

  return g_bytes_new_take (g_steal_pointer (&contents), length);
}
