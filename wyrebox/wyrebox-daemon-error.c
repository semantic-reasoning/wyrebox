#include "wyrebox-daemon-error.h"

typedef struct
{
  WyreboxDaemonErrorClass error_class;
  const char *wire_name;
} ErrorClassMapping;

static const ErrorClassMapping error_class_mappings[] = {
  {WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE, "temporaryFailure"},
  {WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE, "permanentFailure"},
  {WYREBOX_DAEMON_ERROR_PERMISSION_DENIED, "permissionDenied"},
  {WYREBOX_DAEMON_ERROR_NOT_FOUND, "notFound"},
  {WYREBOX_DAEMON_ERROR_CONFLICT, "conflict"},
  {WYREBOX_DAEMON_ERROR_INTERNAL_ERROR, "internalError"},
};

const char *
wyrebox_daemon_error_class_to_string (WyreboxDaemonErrorClass error_class)
{
  for (guint index = 0; index < G_N_ELEMENTS (error_class_mappings); index++) {
    if (error_class_mappings[index].error_class == error_class)
      return error_class_mappings[index].wire_name;
  }

  return NULL;
}

gboolean
wyrebox_daemon_error_class_from_string (const char *value,
    WyreboxDaemonErrorClass *out_error_class)
{
  g_return_val_if_fail (out_error_class != NULL, FALSE);

  if (value == NULL)
    return FALSE;

  for (guint index = 0; index < G_N_ELEMENTS (error_class_mappings); index++) {
    if (g_strcmp0 (error_class_mappings[index].wire_name, value) == 0) {
      *out_error_class = error_class_mappings[index].error_class;
      return TRUE;
    }
  }

  return FALSE;
}

WyreboxDaemonErrorClass
wyrebox_daemon_error_class_from_g_error_code (GIOErrorEnum code)
{
  switch (code) {
    case G_IO_ERROR_PERMISSION_DENIED:
      return WYREBOX_DAEMON_ERROR_PERMISSION_DENIED;
    case G_IO_ERROR_NOT_FOUND:
      return WYREBOX_DAEMON_ERROR_NOT_FOUND;
    case G_IO_ERROR_EXISTS:
      return WYREBOX_DAEMON_ERROR_CONFLICT;
    case G_IO_ERROR_INVALID_ARGUMENT:
    case G_IO_ERROR_INVALID_DATA:
    case G_IO_ERROR_NOT_SUPPORTED:
      return WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE;
    case G_IO_ERROR_TIMED_OUT:
    case G_IO_ERROR_BUSY:
    case G_IO_ERROR_WOULD_BLOCK:
      return WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE;
    default:
      return WYREBOX_DAEMON_ERROR_INTERNAL_ERROR;
  }
}
