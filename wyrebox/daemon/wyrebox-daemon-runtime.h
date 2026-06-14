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

gboolean wyrebox_daemon_runtime_prepare_catalog (
    const char *catalog_path,
    gboolean checkpoint_precondition_satisfied,
    GError **error);

typedef struct _WyreboxDaemonFactMutationService
    WyreboxDaemonFactMutationService;

gboolean wyrebox_daemon_runtime_catch_up_configured_wirelog_derived_views (
    WyreboxDaemonFactMutationService *fact_mutation_service,
    const char *catalog_path,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
