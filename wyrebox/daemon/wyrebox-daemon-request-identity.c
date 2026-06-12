#include "wyrebox-daemon-request-identity.h"

#include <gio/gio.h>

static char *
copy_optional_identity (const char *value)
{
  if (value == NULL || *value == '\0')
    return NULL;

  return g_strdup (value);
}

void
wyrebox_daemon_request_identity_clear (WyreboxDaemonRequestIdentity *identity)
{
  if (identity == NULL)
    return;

  g_clear_pointer (&identity->request_id, g_free);
  g_clear_pointer (&identity->caller_identity, g_free);
  g_clear_pointer (&identity->account_identity, g_free);
  g_clear_pointer (&identity->tool_identity, g_free);
  g_clear_pointer (&identity->correlation_id, g_free);
}

gboolean
wyrebox_daemon_request_identity_init (WyreboxDaemonRequestIdentity *identity,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *tool_identity, const char *correlation_id, GError **error)
{
  g_auto (WyreboxDaemonRequestIdentity) next = { 0 };

  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (request_id == NULL || *request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "request identity requires request_id");
    return FALSE;
  }

  next.request_id = g_strdup (request_id);
  next.caller_identity = copy_optional_identity (caller_identity);
  next.account_identity = copy_optional_identity (account_identity);
  next.tool_identity = copy_optional_identity (tool_identity);
  next.correlation_id = copy_optional_identity (correlation_id);

  wyrebox_daemon_request_identity_clear (identity);
  *identity = next;
  next.request_id = NULL;
  next.caller_identity = NULL;
  next.account_identity = NULL;
  next.tool_identity = NULL;
  next.correlation_id = NULL;

  return TRUE;
}
