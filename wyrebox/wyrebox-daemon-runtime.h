#pragma once

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_DAEMON_DEFAULT_RUNTIME_DIR "/run/wyrebox"
#define WYREBOX_DAEMON_DEFAULT_SOCKET_PATH "/run/wyrebox/wyrebox.sock"

const char *wyrebox_daemon_runtime_get_default_runtime_dir (void);

const char *wyrebox_daemon_runtime_get_default_socket_path (void);

G_END_DECLS
/* *INDENT-ON* */
