#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-uds-client.h"
#include "wyrebox-postfix-pipe-input.h"
#include "wyrebox-postfix-pipe-outcome.h"

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <sysexits.h>

static gboolean
init_local_error_decision (WyreboxPostfixPipeDecision *decision,
    const WyreboxDaemonRequestIdentity *identity, const GError *cause)
{
  return wyrebox_postfix_pipe_decision_init_from_local_error (decision,
      identity != NULL ? identity->request_id : NULL,
      identity != NULL ? identity->correlation_id : NULL, cause, NULL);
}

static int
return_local_error (const WyreboxDaemonRequestIdentity *identity,
    const GError *cause)
{
  g_auto (WyreboxPostfixPipeDecision) decision = { 0 };

  if (!init_local_error_decision (&decision, identity, cause)) {
    g_printerr ("postfix_pipe outcome=temporary_failure exit_status=%d\n",
        EX_TEMPFAIL);
    return EX_TEMPFAIL;
  }

  g_printerr ("%s\n", decision.log_message);
  return decision.exit_status;
}

int
main (int argc, char **argv)
{
  g_autoptr (GInputStream) stdin_stream = NULL;
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxPostfixPipeDecision) decision = { 0 };
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response_payload = NULL;
  g_autoptr (GError) error = NULL;

  stdin_stream = g_unix_input_stream_new (STDIN_FILENO, FALSE);
  if (!wyrebox_postfix_pipe_input_build (argc,
          (const char *const *) argv,
          stdin_stream, G_MAXSIZE, &options, &identity, &request, &error))
    return return_local_error (NULL, error);

  request_payload =
      wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (&identity,
      &request, NULL, &error);
  if (request_payload == NULL)
    return return_local_error (&identity, error);

  response_payload =
      wyrebox_daemon_uds_client_send_request (options.socket_path,
      request_payload, &error);
  if (response_payload == NULL)
    return return_local_error (&identity, error);

  if (!wyrebox_daemon_capnp_codec_decode_response_frame (response_payload,
          &response, &error))
    return return_local_error (&identity, error);

  if (!wyrebox_postfix_pipe_decision_init_from_response (&decision,
          &response, &error))
    return return_local_error (&identity, error);

  g_printerr ("%s\n", decision.log_message);
  return decision.exit_status;
}
