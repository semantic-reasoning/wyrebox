#include "wyrebox-dovecot-daemon-client.h"

#include "wyrebox-build-config.h"
#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-mailbox-select-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"
#include "wyrebox-daemon-uds-client.h"

#include <gio/gio.h>

#define WYREBOX_DOVECOT_CALLER_IDENTITY "dovecot"
#define WYREBOX_DOVECOT_TOOL_IDENTITY "dovecot-storage"

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION

static gboolean
validate_select_inputs (const char *socket_path,
    const char *account_identity, const char *mailbox_name, GError **error)
{
  if (socket_path == NULL || socket_path[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires a non-empty socket path");
    return FALSE;
  }

  if (account_identity == NULL || account_identity[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires a non-empty account identity");
    return FALSE;
  }

  if (mailbox_name == NULL || mailbox_name[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires a non-empty mailbox name");
    return FALSE;
  }

  return TRUE;
}

static gboolean
set_daemon_error_response (const WyreboxDaemonResponseFrame *response,
    GError **error)
{
  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "daemon mailbox SELECT failed: %s",
      response->error.message != NULL ? response->error.message :
      "daemon returned an error response");
  return FALSE;
}

static gboolean
copy_mailbox_select_response (const WyreboxDaemonResponseFrame *response,
    WyreboxDaemonMailboxSelectResult *out_result, GError **error)
{
  return wyrebox_daemon_mailbox_select_result_init (out_result,
      response->mailbox_select.kind,
      response->mailbox_select.mailbox_id,
      response->mailbox_select.mailbox_name,
      response->mailbox_select.uid_validity,
      response->mailbox_select.uid_next, error);
}

gboolean
wyrebox_dovecot_daemon_client_select_mailbox (const char *socket_path,
    const char *account_identity,
    const char *mailbox_name,
    WyreboxDaemonMailboxSelectResult *out_result, GError **error)
{
  g_autofree char *request_id = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response_payload = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (out_result == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires an output mailbox SELECT result");
    return FALSE;
  }

  wyrebox_daemon_mailbox_select_result_clear (out_result);

  if (!validate_select_inputs (socket_path, account_identity, mailbox_name,
          error))
    return FALSE;

  request_id = g_uuid_string_random ();
  if (!wyrebox_daemon_request_identity_init (&identity,
          request_id,
          WYREBOX_DOVECOT_CALLER_IDENTITY,
          account_identity, WYREBOX_DOVECOT_TOOL_IDENTITY, NULL, error))
    return FALSE;

  if (!wyrebox_daemon_mailbox_select_request_init (&request,
          account_identity, NULL, mailbox_name, error))
    return FALSE;

  request_payload =
      wyrebox_daemon_capnp_codec_encode_mailbox_select_request (&identity,
      &request, NULL, error);
  if (request_payload == NULL)
    return FALSE;

  response_payload =
      wyrebox_daemon_uds_client_send_request (socket_path, request_payload,
      error);
  if (response_payload == NULL)
    return FALSE;

  if (!wyrebox_daemon_capnp_codec_decode_response_frame (response_payload,
          &response, error))
    return FALSE;

  switch (response.kind) {
    case WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_SELECT:
      return copy_mailbox_select_response (&response, out_result, error);
    case WYREBOX_DAEMON_RESPONSE_FRAME_ERROR:
      return set_daemon_error_response (&response, error);
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "daemon returned unexpected response kind %d for mailbox SELECT",
          response.kind);
      return FALSE;
  }
}

#else

gboolean
wyrebox_dovecot_daemon_client_select_mailbox (const char *socket_path,
    const char *account_identity,
    const char *mailbox_name,
    WyreboxDaemonMailboxSelectResult *out_result, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (out_result != NULL)
    wyrebox_daemon_mailbox_select_result_clear (out_result);

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "Dovecot daemon client requires Cap'n Proto serialization support");
  return FALSE;
}

#endif
