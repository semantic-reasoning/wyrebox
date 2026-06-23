#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_DAEMON_DEFAULT_CONFIG_DIR "/etc/wyrebox"
#define WYREBOX_DAEMON_DEFAULT_CONFIG_PATH "/etc/wyrebox/wyrebox.conf"
#define WYREBOX_DAEMON_DEFAULT_JOURNAL_ROOT_DIR "/var/lib/wyrebox/journal"
#define WYREBOX_DAEMON_DEFAULT_OBJECT_ROOT_DIR "/var/lib/wyrebox/object-store"

#define WYREBOX_TYPE_DAEMON_CONFIG (wyrebox_daemon_config_get_type())
G_DECLARE_FINAL_TYPE (WyreboxDaemonConfig, wyrebox_daemon_config, WYREBOX,
    DAEMON_CONFIG, GObject)

/*
 * Returns: (transfer full): a new daemon config loaded and validated from
 * @config_path.
 */
WyreboxDaemonConfig *wyrebox_daemon_config_new_from_file (const char
    *config_path, GError **error);

/*
 * Returns: whether @self satisfies daemon startup invariants.
 */
gboolean wyrebox_daemon_config_validate_for_startup (const WyreboxDaemonConfig
    *self, GError **error);

/*
 * Returns: (transfer none): the validated socket path.
 */
const char *wyrebox_daemon_config_get_socket_path (WyreboxDaemonConfig *self);

/*
 * Returns: (transfer none): the validated journal root directory.
 */
const char *wyrebox_daemon_config_get_journal_root_dir (WyreboxDaemonConfig *self);

/*
 * Returns: (transfer none): the validated object store root directory.
 */
const char *wyrebox_daemon_config_get_object_root_dir (WyreboxDaemonConfig *self);

/*
 * Returns: (transfer none): the validated config file path.
 */
const char *wyrebox_daemon_config_get_config_path (WyreboxDaemonConfig *self);

G_END_DECLS
/* *INDENT-ON* */
