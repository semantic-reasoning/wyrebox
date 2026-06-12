#pragma once

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum {
  WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE,
  WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE,
  WYREBOX_DAEMON_ERROR_PERMISSION_DENIED,
  WYREBOX_DAEMON_ERROR_NOT_FOUND,
  WYREBOX_DAEMON_ERROR_CONFLICT,
  WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
} WyreboxDaemonErrorClass;

const char *wyrebox_daemon_error_class_to_string (
    WyreboxDaemonErrorClass error_class);

gboolean wyrebox_daemon_error_class_from_string (
    const char *value,
    WyreboxDaemonErrorClass *out_error_class);

WyreboxDaemonErrorClass wyrebox_daemon_error_class_from_g_error_code (
    GIOErrorEnum code);

G_END_DECLS
/* *INDENT-ON* */
