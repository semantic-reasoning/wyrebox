#include "wyrebox-daemon-mailbox-list-request.h"

#include <gio/gio.h>
#include <string.h>

static gboolean
validate_required_text (const char *value,
    const char *field_name, GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "mailbox LIST %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "mailbox LIST %s must not contain control characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_optional_text (const char *value,
    const char *field_name, GError **error)
{
  if (value == NULL)
    return TRUE;

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "mailbox LIST %s must not contain control characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

void
wyrebox_daemon_mailbox_list_request_clear (WyreboxDaemonMailboxListRequest
    *request)
{
  if (request == NULL)
    return;

  g_clear_pointer (&request->account_identity, g_free);
  g_clear_pointer (&request->namespace_prefix, g_free);
}

gboolean
wyrebox_daemon_mailbox_list_request_init (WyreboxDaemonMailboxListRequest
    *request, const char *account_identity, const char *namespace_prefix,
    GError **error)
{
  g_auto (WyreboxDaemonMailboxListRequest) next = { 0 };

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_text (account_identity, "account_identity", error))
    return FALSE;

  if (!validate_optional_text (namespace_prefix, "namespace_prefix", error))
    return FALSE;

  next.account_identity = g_strdup (account_identity);
  next.namespace_prefix = g_strdup (namespace_prefix != NULL ?
      namespace_prefix : "");

  wyrebox_daemon_mailbox_list_request_clear (request);
  *request = next;
  next.account_identity = NULL;
  next.namespace_prefix = NULL;

  return TRUE;
}
