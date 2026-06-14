#include "wyrebox-postfix-lmtp-delivery-bridge.h"

#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-uds-client.h"

static gboolean
set_local_delivery_reply (WyreboxPostfixLmtpReply *reply,
    const WyreboxDaemonRequestIdentity *identity, const GError *cause,
    GError **error)
{
  return wyrebox_postfix_lmtp_reply_init_from_delivery_error (reply,
      identity != NULL ? identity->request_id : NULL,
      identity != NULL ? identity->correlation_id : NULL, cause, error);
}

static gboolean
validate_input (const WyreboxPostfixLmtpOptions *options,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxPostfixLmtpReply *reply, GError **error)
{
  if (options == NULL || options->socket_path == NULL ||
      *options->socket_path == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "LMTP delivery socket path is required");
    return FALSE;
  }

  if (identity == NULL || identity->request_id == NULL ||
      *identity->request_id == '\0' || identity->correlation_id == NULL ||
      *identity->correlation_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "LMTP delivery request identity is required");
    return FALSE;
  }

  if (request == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "LMTP delivery request is required");
    return FALSE;
  }

  return TRUE;
}

static gboolean
response_matches_identity (const WyreboxDaemonResponseFrame *response,
    const WyreboxDaemonRequestIdentity *identity)
{
  return g_strcmp0 (response->request_id, identity->request_id) == 0 &&
      g_strcmp0 (response->correlation_id, identity->correlation_id) == 0;
}

gboolean
wyrebox_postfix_lmtp_delivery_bridge_deliver (const WyreboxPostfixLmtpOptions
    *options, const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxPostfixLmtpReply *reply, GError **error)
{
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response_payload = NULL;
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (reply != NULL, FALSE);

  if (!validate_input (options, identity, request, reply, &local_error))
    return set_local_delivery_reply (reply, identity, local_error, error);

  request_payload =
      wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (identity,
      request, NULL, &local_error);
  if (request_payload == NULL)
    return set_local_delivery_reply (reply, identity, local_error, error);

  response_payload =
      wyrebox_daemon_uds_client_send_request (options->socket_path,
      request_payload, &local_error);
  if (response_payload == NULL)
    return set_local_delivery_reply (reply, identity, local_error, error);

  if (!wyrebox_daemon_capnp_codec_decode_response_frame (response_payload,
          &response, &local_error))
    return set_local_delivery_reply (reply, identity, local_error, error);

  if (!response_matches_identity (&response, identity)) {
    local_error = g_error_new (G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon response identity does not match LMTP delivery request");
    return set_local_delivery_reply (reply, identity, local_error, error);
  }

  if (!wyrebox_postfix_lmtp_reply_init_from_response (reply,
          &response, &local_error))
    return set_local_delivery_reply (reply, identity, local_error, error);

  return TRUE;
}
