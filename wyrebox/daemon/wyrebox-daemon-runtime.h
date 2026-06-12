#pragma once

#include <gio/gio.h>
#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_DAEMON_DEFAULT_RUNTIME_DIR "/run/wyrebox"
#define WYREBOX_DAEMON_DEFAULT_SOCKET_PATH "/run/wyrebox/wyrebox.sock"
#define WYREBOX_DAEMON_DEFAULT_FACT_DUMP_DIR "/run/wyrebox/facts"

const char *wyrebox_daemon_runtime_get_default_runtime_dir (void);

const char *wyrebox_daemon_runtime_get_default_socket_path (void);

const char *wyrebox_daemon_runtime_get_default_fact_dump_dir (void);

/*
 * Returns: (transfer full): caller-owned runtime fact dump directory file.
 * Free with g_object_unref().
 */
GFile *wyrebox_daemon_runtime_get_default_fact_dump_file (void);

G_END_DECLS
/* *INDENT-ON* */
