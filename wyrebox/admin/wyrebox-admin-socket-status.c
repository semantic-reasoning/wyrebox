#include "wyrebox-admin-socket-status.h"

#include "wyrebox-daemon-runtime.h"

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

typedef enum
{
  WYREBOX_ADMIN_SOCKET_STATUS_STATE_OK,
  WYREBOX_ADMIN_SOCKET_STATUS_STATE_MISSING_PATH,
  WYREBOX_ADMIN_SOCKET_STATUS_STATE_NOT_SOCKET,
  WYREBOX_ADMIN_SOCKET_STATUS_STATE_CONNECT_FAILED,
} WyreboxAdminSocketStatusState;

static const char *
socket_status_state_to_name (WyreboxAdminSocketStatusState state)
{
  switch (state) {
    case WYREBOX_ADMIN_SOCKET_STATUS_STATE_OK:
      return "ok";
    case WYREBOX_ADMIN_SOCKET_STATUS_STATE_MISSING_PATH:
      return "missing-path";
    case WYREBOX_ADMIN_SOCKET_STATUS_STATE_NOT_SOCKET:
      return "not-socket";
    case WYREBOX_ADMIN_SOCKET_STATUS_STATE_CONNECT_FAILED:
      return "connect-failed";
    default:
      return "unknown";
  }
}

const char *
wyrebox_admin_socket_status_default_socket_path (void)
{
  return wyrebox_daemon_runtime_get_default_socket_path ();
}

void
wyrebox_admin_socket_status_result_clear (WyreboxAdminSocketStatusResult
    *result)
{
  if (result == NULL)
    return;

  g_clear_pointer (&result->socket_path, g_free);
  g_clear_pointer (&result->status_name, g_free);
  result->connectable = FALSE;
}

static gboolean
socket_path_is_socket (const char *socket_path)
{
  struct stat st = { 0 };

  if (g_stat (socket_path, &st) != 0)
    return FALSE;

  return S_ISSOCK (st.st_mode);
}

static gboolean
socket_path_connectable (const char *socket_path)
{
  g_autoptr (GSocketClient) client = NULL;
  g_autoptr (GSocketAddress) address = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

  client = g_socket_client_new ();
  address = g_unix_socket_address_new (socket_path);
  connection = g_socket_client_connect (client,
      G_SOCKET_CONNECTABLE (address), NULL, &error);
  if (connection == NULL)
    return FALSE;

  return TRUE;
}

int
wyrebox_admin_socket_status_probe (const char *socket_path,
    WyreboxAdminSocketStatusResult *result)
{
  WyreboxAdminSocketStatusState state =
      WYREBOX_ADMIN_SOCKET_STATUS_STATE_CONNECT_FAILED;
  int exit_status = WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED;

  g_return_val_if_fail (socket_path != NULL,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR);
  g_return_val_if_fail (*socket_path != '\0',
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR);
  g_return_val_if_fail (result != NULL,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR);

  wyrebox_admin_socket_status_result_clear (result);
  result->socket_path = g_strdup (socket_path);

  if (!g_file_test (socket_path, G_FILE_TEST_EXISTS)) {
    state = WYREBOX_ADMIN_SOCKET_STATUS_STATE_MISSING_PATH;
    exit_status = WYREBOX_ADMIN_SOCKET_STATUS_EXIT_MISSING_PATH;
  } else if (!socket_path_is_socket (socket_path)) {
    state = WYREBOX_ADMIN_SOCKET_STATUS_STATE_NOT_SOCKET;
    exit_status = WYREBOX_ADMIN_SOCKET_STATUS_EXIT_NOT_SOCKET;
  } else if (socket_path_connectable (socket_path)) {
    state = WYREBOX_ADMIN_SOCKET_STATUS_STATE_OK;
    exit_status = WYREBOX_ADMIN_SOCKET_STATUS_EXIT_SUCCESS;
  }

  result->status_name = g_strdup (socket_status_state_to_name (state));
  result->connectable = (state == WYREBOX_ADMIN_SOCKET_STATUS_STATE_OK);
  return exit_status;
}
