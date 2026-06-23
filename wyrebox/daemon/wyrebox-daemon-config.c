#include "wyrebox-daemon-config.h"
#include "wyrebox-daemon-runtime.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

struct _WyreboxDaemonConfig
{
  GObject parent_instance;

  char *config_path;
  char *socket_path;
  char *journal_root_dir;
  char *object_root_dir;
  char *catalog_path;
};

G_DEFINE_TYPE (WyreboxDaemonConfig, wyrebox_daemon_config, G_TYPE_OBJECT);

static gboolean
config_file_is_secure (const char *config_path, GError **error)
{
  struct stat stat_buf = { 0 };

  if (lstat (config_path, &stat_buf) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to stat daemon config '%s': %s",
        config_path, g_strerror (saved_errno));
    return FALSE;
  }

  if (!S_ISREG (stat_buf.st_mode)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon config '%s' is not a regular file", config_path);
    return FALSE;
  }

  if ((stat_buf.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "daemon config '%s' must not be group- or world-writable", config_path);
    return FALSE;
  }

  return TRUE;
}

static gboolean
config_line_is_comment_or_blank (const char *line)
{
  const char *cursor = line;

  while (cursor != NULL && g_ascii_isspace (*cursor))
    cursor++;

  return cursor == NULL || *cursor == '\0' || *cursor == '#' || *cursor == ';';
}

static gboolean
parse_daemon_config_file (const char *config_path, const char *contents,
    char **out_socket_path, char **out_journal_root_dir,
    char **out_object_root_dir, char **out_catalog_path, GError **error)
{
  g_auto (GStrv) lines = NULL;
  gboolean in_daemon_section = FALSE;
  gboolean seen_socket_path = FALSE;
  gboolean seen_journal_root_dir = FALSE;
  gboolean seen_object_root_dir = FALSE;
  gboolean seen_catalog_path = FALSE;
  char *socket_path = NULL;
  char *journal_root_dir = NULL;
  char *object_root_dir = NULL;
  char *catalog_path = NULL;

  lines = g_strsplit (contents, "\n", -1);

  for (guint index = 0; lines[index] != NULL; index++) {
    char *line = lines[index];
    char *separator = NULL;
    char *key = NULL;
    char *value = NULL;

    g_strstrip (line);
    if (config_line_is_comment_or_blank (line))
      continue;

    if (line[0] == '[') {
      char *section_end = strchr (line, ']');

      if (section_end == NULL || section_end[1] != '\0') {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config '%s' has malformed section header: %s",
            config_path, line);
        goto out;
      }

      *section_end = '\0';
      in_daemon_section = g_strcmp0 (line + 1, "daemon") == 0;
      if (!in_daemon_section) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config '%s' has unsupported section '%s'",
            config_path, line + 1);
        goto out;
      }

      continue;
    }

    if (!in_daemon_section) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "daemon config '%s' must define a [daemon] section before %s",
          config_path, line);
      goto out;
    }

    separator = strchr (line, '=');
    if (separator == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "daemon config '%s' has malformed assignment: %s", config_path, line);
      goto out;
    }

    *separator = '\0';
    key = g_strstrip (line);
    value = g_strstrip (separator + 1);

    if (key == NULL || *key == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "daemon config '%s' has an empty key near %s",
          config_path, separator + 1);
      goto out;
    }

    if (g_strcmp0 (key, "socket_path") == 0) {
      if (seen_socket_path) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config '%s' defines socket_path more than once",
            config_path);
        goto out;
      }

      socket_path = g_strdup (value);
      seen_socket_path = TRUE;
      continue;
    }

    if (g_strcmp0 (key, "journal_root_dir") == 0) {
      if (seen_journal_root_dir) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config '%s' defines journal_root_dir more than once",
            config_path);
        goto out;
      }

      journal_root_dir = g_strdup (value);
      seen_journal_root_dir = TRUE;
      continue;
    }

    if (g_strcmp0 (key, "object_root_dir") == 0) {
      if (seen_object_root_dir) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config '%s' defines object_root_dir more than once",
            config_path);
        goto out;
      }

      object_root_dir = g_strdup (value);
      seen_object_root_dir = TRUE;
      continue;
    }

    if (g_strcmp0 (key, "catalog_path") == 0) {
      if (seen_catalog_path) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config '%s' defines catalog_path more than once",
            config_path);
        goto out;
      }

      catalog_path = g_strdup (value);
      seen_catalog_path = TRUE;
      continue;
    }

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon config '%s' has unknown key '%s'", config_path, key);
    goto out;
  }

  if (!seen_socket_path) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon config '%s' is missing socket_path", config_path);
    goto out;
  }

  *out_socket_path = g_steal_pointer (&socket_path);
  *out_journal_root_dir = g_steal_pointer (&journal_root_dir);
  *out_object_root_dir = g_steal_pointer (&object_root_dir);
  *out_catalog_path = g_steal_pointer (&catalog_path);
  return TRUE;

out:
  g_clear_pointer (&socket_path, g_free);
  g_clear_pointer (&journal_root_dir, g_free);
  g_clear_pointer (&object_root_dir, g_free);
  g_clear_pointer (&catalog_path, g_free);
  return FALSE;
}

static void
wyrebox_daemon_config_finalize (GObject *object)
{
  WyreboxDaemonConfig *self = WYREBOX_DAEMON_CONFIG (object);

  g_clear_pointer (&self->config_path, g_free);
  g_clear_pointer (&self->socket_path, g_free);
  g_clear_pointer (&self->journal_root_dir, g_free);
  g_clear_pointer (&self->object_root_dir, g_free);
  g_clear_pointer (&self->catalog_path, g_free);

  G_OBJECT_CLASS (wyrebox_daemon_config_parent_class)->finalize (object);
}

static void
wyrebox_daemon_config_class_init (WyreboxDaemonConfigClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_config_finalize;
}

static void
wyrebox_daemon_config_init (WyreboxDaemonConfig *self)
{
}

gboolean
wyrebox_daemon_config_validate_for_startup (const WyreboxDaemonConfig *self,
    GError **error)
{
  const char *socket_path = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self == NULL) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "daemon config is required");
    return FALSE;
  }

  socket_path = self->socket_path;
  if (socket_path == NULL || *socket_path == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "daemon config socket_path is required");
    return FALSE;
  }

  if (!g_path_is_absolute (socket_path)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon config socket_path must be absolute: %s", socket_path);
    return FALSE;
  }

  if (self->journal_root_dir == NULL || *self->journal_root_dir == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "daemon config journal_root_dir is required");
    return FALSE;
  }

  if (!g_path_is_absolute (self->journal_root_dir)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon config journal_root_dir must be absolute: %s",
        self->journal_root_dir);
    return FALSE;
  }

  if (self->object_root_dir == NULL || *self->object_root_dir == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "daemon config object_root_dir is required");
    return FALSE;
  }

  if (!g_path_is_absolute (self->object_root_dir)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon config object_root_dir must be absolute: %s",
        self->object_root_dir);
    return FALSE;
  }

  if (self->catalog_path == NULL || *self->catalog_path == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "daemon config catalog_path is required");
    return FALSE;
  }

  if (!g_path_is_absolute (self->catalog_path)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon config catalog_path must be absolute: %s", self->catalog_path);
    return FALSE;
  }

  return TRUE;
}

WyreboxDaemonConfig *
wyrebox_daemon_config_new_from_file (const char *config_path, GError **error)
{
  g_autofree char *contents = NULL;
  g_autofree char *socket_path = NULL;
  g_autofree char *journal_root_dir = NULL;
  g_autofree char *object_root_dir = NULL;
  g_autofree char *catalog_path = NULL;
  WyreboxDaemonConfig *self = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (config_path == NULL || *config_path == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "daemon config path is required");
    return NULL;
  }

  if (!g_path_is_absolute (config_path)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "daemon config path must be absolute: %s", config_path);
    return NULL;
  }

  if (!config_file_is_secure (config_path, error))
    return NULL;

  if (!g_file_get_contents (config_path, &contents, NULL, error))
    return NULL;

  if (!parse_daemon_config_file (config_path, contents,
          &socket_path, &journal_root_dir, &object_root_dir, &catalog_path,
          error))
    return NULL;

  self = g_object_new (WYREBOX_TYPE_DAEMON_CONFIG, NULL);
  self->config_path = g_strdup (config_path);
  self->socket_path = g_steal_pointer (&socket_path);
  self->journal_root_dir = g_steal_pointer (&journal_root_dir);
  self->object_root_dir = g_steal_pointer (&object_root_dir);
  self->catalog_path = g_steal_pointer (&catalog_path);

  if (self->journal_root_dir == NULL)
    self->journal_root_dir = g_strdup (WYREBOX_DAEMON_DEFAULT_JOURNAL_ROOT_DIR);
  if (self->object_root_dir == NULL)
    self->object_root_dir = g_strdup (WYREBOX_DAEMON_DEFAULT_OBJECT_ROOT_DIR);
  if (self->catalog_path == NULL)
    self->catalog_path = g_strdup (WYREBOX_DAEMON_DEFAULT_CATALOG_PATH);

  if (!wyrebox_daemon_config_validate_for_startup (self, error)) {
    g_object_unref (self);
    self = NULL;
    return NULL;
  }

  return self;
}

const char *
wyrebox_daemon_config_get_socket_path (WyreboxDaemonConfig *self)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_CONFIG (self), NULL);

  return self->socket_path;
}

const char *
wyrebox_daemon_config_get_config_path (WyreboxDaemonConfig *self)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_CONFIG (self), NULL);

  return self->config_path;
}

const char *
wyrebox_daemon_config_get_journal_root_dir (WyreboxDaemonConfig *self)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_CONFIG (self), NULL);

  return self->journal_root_dir;
}

const char *
wyrebox_daemon_config_get_object_root_dir (WyreboxDaemonConfig *self)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_CONFIG (self), NULL);

  return self->object_root_dir;
}

const char *
wyrebox_daemon_config_get_catalog_path (WyreboxDaemonConfig *self)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_CONFIG (self), NULL);

  return self->catalog_path;
}
