#include "wyrebox-admin-health.h"

#include "wyrebox-admin-socket-status.h"

#include <glib/gstdio.h>

void
wyrebox_admin_health_options_clear (WyreboxAdminHealthOptions *options)
{
  if (options == NULL)
    return;

  g_clear_pointer (&options->socket_path, g_free);
  options->json = FALSE;
}

void
wyrebox_admin_health_result_clear (WyreboxAdminHealthResult *result)
{
  if (result == NULL)
    return;

  g_clear_pointer (&result->socket_path, g_free);
  g_clear_pointer (&result->status_name, g_free);
  result->healthy = FALSE;
}

const char *
wyrebox_admin_health_default_socket_path (void)
{
  return wyrebox_admin_socket_status_default_socket_path ();
}

int
wyrebox_admin_health_probe (const char *socket_path,
    WyreboxAdminHealthResult *result)
{
  WyreboxAdminSocketStatusResult socket_status = { 0 };
  int exit_status = WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED;

  g_return_val_if_fail (socket_path != NULL,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR);
  g_return_val_if_fail (*socket_path != '\0',
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR);
  g_return_val_if_fail (result != NULL,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR);

  wyrebox_admin_health_result_clear (result);
  result->socket_path = g_strdup (socket_path);

  exit_status = wyrebox_admin_socket_status_probe (socket_path, &socket_status);
  result->status_name = g_strdup (socket_status.status_name);
  result->healthy = socket_status.connectable;

  wyrebox_admin_socket_status_result_clear (&socket_status);
  return exit_status;
}
