#include "wyrebox-daemon-runtime.h"

const char *
wyrebox_daemon_runtime_get_default_runtime_dir (void)
{
  return WYREBOX_DAEMON_DEFAULT_RUNTIME_DIR;
}

const char *
wyrebox_daemon_runtime_get_default_socket_path (void)
{
  return WYREBOX_DAEMON_DEFAULT_SOCKET_PATH;
}
