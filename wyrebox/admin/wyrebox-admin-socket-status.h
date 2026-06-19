#pragma once

#include <gio/gio.h>
#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_ADMIN_SOCKET_STATUS_EXIT_SUCCESS 0
#define WYREBOX_ADMIN_SOCKET_STATUS_EXIT_MISSING_PATH 66
#define WYREBOX_ADMIN_SOCKET_STATUS_EXIT_NOT_SOCKET 72
#define WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED 75
#define WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR 64

typedef struct
{
  char *socket_path;
  char *status_name;
  gboolean connectable;
} WyreboxAdminSocketStatusResult;

const char *wyrebox_admin_socket_status_default_socket_path (void);

void wyrebox_admin_socket_status_result_clear (
    WyreboxAdminSocketStatusResult *result);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxAdminSocketStatusResult,
    wyrebox_admin_socket_status_result_clear)

int wyrebox_admin_socket_status_probe (const char *socket_path,
    WyreboxAdminSocketStatusResult *result);

G_END_DECLS
/* *INDENT-ON* */
