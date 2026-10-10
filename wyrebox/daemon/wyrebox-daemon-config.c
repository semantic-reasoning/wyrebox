#include "wyrebox-daemon-config.h"
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-derived-view-imap-name.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

typedef struct
{
    char *view_id;
    char *imap_name;
} ConfigView;

struct _WyreboxDaemonConfig
{
    GObject parent_instance;

    char *config_path;
    char *socket_path;
    char *journal_root_dir;
    char *object_root_dir;
    char *catalog_path;
    char *wirelog_rules_path;
    char *extraction_rules_path;
    GPtrArray *views;
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
            "daemon config '%s' must not be group- or world-writable",
            config_path);
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

    return cursor == NULL || *cursor == '\0' || *cursor == '#' ||
           *cursor == ';';
}

typedef enum
{
    CONFIG_SECTION_NONE,
    CONFIG_SECTION_DAEMON,
    CONFIG_SECTION_WIRELOG,
    CONFIG_SECTION_VIEW,
} ConfigSection;

static void
config_view_free (gpointer data)
{
    ConfigView *view = data;

    g_free (view->view_id);
    g_free (view->imap_name);
    g_free (view);
}

static gboolean
view_id_is_valid (const char *view_id)
{
    if (*view_id == '\0')
        return FALSE;

    for (const char *cursor = view_id; *cursor != '\0'; cursor++) {
        if (!g_ascii_isalnum (*cursor) && *cursor != '-' && *cursor != '_' &&
            *cursor != '.')
            return FALSE;
    }

    return TRUE;
}

static gboolean
assign_once (const char *config_path, const char *section, const char *key,
    const char *value, char **slot, GError **error)
{
    if (*slot != NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config '%s' defines %s %s more than once",
            config_path, section, key);
        return FALSE;
    }

    *slot = g_strdup (value);
    return TRUE;
}

static gboolean
begin_section (WyreboxDaemonConfig *self, const char *config_path,
    const char *name, ConfigSection *section, ConfigView **view,
    GError **error)
{
    *view = NULL;

    if (g_strcmp0 (name, "daemon") == 0) {
        *section = CONFIG_SECTION_DAEMON;
        return TRUE;
    }

    if (g_strcmp0 (name, "wirelog") == 0) {
        *section = CONFIG_SECTION_WIRELOG;
        return TRUE;
    }

    if (g_str_has_prefix (name, "view:")) {
        const char *view_id = name + strlen ("view:");

        if (!view_id_is_valid (view_id)) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "daemon config '%s' has invalid view id '%s'; use letters, "
                "digits, '-', '_', or '.'", config_path, view_id);
            return FALSE;
        }

        for (guint i = 0; i < self->views->len; i++) {
            const ConfigView *existing = g_ptr_array_index (self->views, i);

            if (g_strcmp0 (existing->view_id, view_id) == 0) {
                g_set_error (error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "daemon config '%s' defines view '%s' more than once",
                    config_path, view_id);
                return FALSE;
            }
        }

        *view = g_new0 (ConfigView, 1);
        (*view)->view_id = g_strdup (view_id);
        g_ptr_array_add (self->views, *view);
        *section = CONFIG_SECTION_VIEW;
        return TRUE;
    }

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon config '%s' has unsupported section '%s'", config_path, name);
    return FALSE;
}

static gboolean
assign_key (WyreboxDaemonConfig *self, const char *config_path,
    ConfigSection section, ConfigView *view, const char *key,
    const char *value, GError **error)
{
    switch (section) {
    case CONFIG_SECTION_DAEMON:
        if (g_strcmp0 (key, "socket_path") == 0)
            return assign_once (config_path, "[daemon]", key, value,
                       &self->socket_path, error);
        if (g_strcmp0 (key, "journal_root_dir") == 0)
            return assign_once (config_path, "[daemon]", key, value,
                       &self->journal_root_dir, error);
        if (g_strcmp0 (key, "object_root_dir") == 0)
            return assign_once (config_path, "[daemon]", key, value,
                       &self->object_root_dir, error);
        if (g_strcmp0 (key, "catalog_path") == 0)
            return assign_once (config_path, "[daemon]", key, value,
                       &self->catalog_path, error);
        break;
    case CONFIG_SECTION_WIRELOG:
        if (g_strcmp0 (key, "rules_path") == 0)
            return assign_once (config_path, "[wirelog]", key, value,
                       &self->wirelog_rules_path, error);
        if (g_strcmp0 (key, "extraction_rules_path") == 0)
            return assign_once (config_path, "[wirelog]", key, value,
                       &self->extraction_rules_path, error);
        break;
    case CONFIG_SECTION_VIEW:
        if (g_strcmp0 (key, "imap_name") == 0)
            return assign_once (config_path, "view", key, value,
                       &view->imap_name, error);
        break;
    case CONFIG_SECTION_NONE:
    default:
        g_assert_not_reached ();
    }

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon config '%s' has unknown key '%s'", config_path, key);
    return FALSE;
}

static gboolean
parse_daemon_config_file (WyreboxDaemonConfig *self, const char *config_path,
    const char *contents, GError **error)
{
    g_auto (GStrv) lines = NULL;
    ConfigSection section = CONFIG_SECTION_NONE;
    ConfigView *view = NULL;

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
                return FALSE;
            }

            *section_end = '\0';
            if (!begin_section (self, config_path, line + 1, &section, &view,
                error))
                return FALSE;

            continue;
        }

        if (section == CONFIG_SECTION_NONE) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "daemon config '%s' must define a [daemon] section before %s",
                config_path, line);
            return FALSE;
        }

        separator = strchr (line, '=');
        if (separator == NULL) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "daemon config '%s' has malformed assignment: %s", config_path,
                line);
            return FALSE;
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
            return FALSE;
        }

        if (!assign_key (self, config_path, section, view, key, value, error))
            return FALSE;
    }

    if (self->socket_path == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config '%s' is missing socket_path", config_path);
        return FALSE;
    }

    return TRUE;
}

static gboolean
validate_wirelog_views (const WyreboxDaemonConfig *self, GError **error)
{
    if (self->wirelog_rules_path != NULL &&
        !g_path_is_absolute (self->wirelog_rules_path)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config [wirelog] rules_path must be absolute: %s",
            self->wirelog_rules_path);
        return FALSE;
    }

    if (self->extraction_rules_path != NULL &&
        !g_path_is_absolute (self->extraction_rules_path)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config [wirelog] extraction_rules_path must be absolute: "
            "%s", self->extraction_rules_path);
        return FALSE;
    }

    if (self->extraction_rules_path != NULL &&
        self->wirelog_rules_path == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config [wirelog] extraction_rules_path requires "
            "rules_path");
        return FALSE;
    }

    if (self->views->len > 0 && self->wirelog_rules_path == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "daemon config view sections require [wirelog] rules_path");
        return FALSE;
    }

    for (guint i = 0; i < self->views->len; i++) {
        const ConfigView *view = g_ptr_array_index (self->views, i);
        g_autoptr (GError) name_error = NULL;

        if (view->imap_name == NULL) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "daemon config view '%s' is missing imap_name", view->view_id);
            return FALSE;
        }

        if (!wyrebox_derived_view_imap_name_validate_stored (view->imap_name,
            &name_error)) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "daemon config view '%s' has invalid imap_name: %s",
                view->view_id, name_error->message);
            return FALSE;
        }

        if (g_ascii_strcasecmp (view->imap_name, "INBOX") == 0) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "daemon config view '%s' has invalid imap_name: INBOX is "
                "reserved", view->view_id);
            return FALSE;
        }

        for (guint j = 0; j < i; j++) {
            const ConfigView *other = g_ptr_array_index (self->views, j);

            if (g_strcmp0 (other->imap_name, view->imap_name) == 0) {
                g_set_error (error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "daemon config views '%s' and '%s' share imap_name '%s'",
                    other->view_id, view->view_id, view->imap_name);
                return FALSE;
            }
        }
    }

    return TRUE;
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
    g_clear_pointer (&self->wirelog_rules_path, g_free);
    g_clear_pointer (&self->extraction_rules_path, g_free);
    g_clear_pointer (&self->views, g_ptr_array_unref);

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
    self->views = g_ptr_array_new_with_free_func (config_view_free);
}

gboolean
wyrebox_daemon_config_validate_for_startup (const WyreboxDaemonConfig *self,
    GError **error)
{
    const char *socket_path = NULL;

    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (self == NULL) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "daemon config is required");
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
            G_IO_ERROR_INVALID_DATA,
            "daemon config journal_root_dir is required");
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
            G_IO_ERROR_INVALID_DATA,
            "daemon config object_root_dir is required");
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
            "daemon config catalog_path must be absolute: %s",
            self->catalog_path);
        return FALSE;
    }

    return validate_wirelog_views (self, error);
}

WyreboxDaemonConfig *
wyrebox_daemon_config_new_from_file (const char *config_path, GError **error)
{
    g_autofree char *contents = NULL;
    g_autoptr (WyreboxDaemonConfig) self = NULL;

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

    self = g_object_new (WYREBOX_TYPE_DAEMON_CONFIG, NULL);
    self->config_path = g_strdup (config_path);
    if (!parse_daemon_config_file (self, config_path, contents, error))
        return NULL;

    if (self->journal_root_dir == NULL)
        self->journal_root_dir =
            g_strdup (WYREBOX_DAEMON_DEFAULT_JOURNAL_ROOT_DIR);
    if (self->object_root_dir == NULL)
        self->object_root_dir =
            g_strdup (WYREBOX_DAEMON_DEFAULT_OBJECT_ROOT_DIR);
    if (self->catalog_path == NULL)
        self->catalog_path = g_strdup (WYREBOX_DAEMON_DEFAULT_CATALOG_PATH);

    if (!wyrebox_daemon_config_validate_for_startup (self, error))
        return NULL;

    return g_steal_pointer (&self);
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

const char *
wyrebox_daemon_config_get_wirelog_rules_path (WyreboxDaemonConfig *self)
{
    g_return_val_if_fail (WYREBOX_IS_DAEMON_CONFIG (self), NULL);

    return self->wirelog_rules_path;
}

const char *
wyrebox_daemon_config_get_extraction_rules_path (WyreboxDaemonConfig *self)
{
    g_return_val_if_fail (WYREBOX_IS_DAEMON_CONFIG (self), NULL);

    return self->extraction_rules_path;
}

guint
wyrebox_daemon_config_get_n_views (WyreboxDaemonConfig *self)
{
    g_return_val_if_fail (WYREBOX_IS_DAEMON_CONFIG (self), 0);

    return self->views->len;
}

const char *
wyrebox_daemon_config_get_view_id (WyreboxDaemonConfig *self, guint index)
{
    const ConfigView *view = NULL;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_CONFIG (self), NULL);
    g_return_val_if_fail (index < self->views->len, NULL);

    view = g_ptr_array_index (self->views, index);
    return view->view_id;
}

const char *
wyrebox_daemon_config_get_view_imap_name (WyreboxDaemonConfig *self,
    guint index)
{
    const ConfigView *view = NULL;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_CONFIG (self), NULL);
    g_return_val_if_fail (index < self->views->len, NULL);

    view = g_ptr_array_index (self->views, index);
    return view->imap_name;
}
