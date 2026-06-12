#include "wyrebox-daemon-request-router.h"

#include "wyrebox-daemon-error-frame.h"

#include <gio/gio.h>

static gboolean
request_id_is_usable (const char *request_id)
{
  return request_id != NULL && *request_id != '\0';
}

static gboolean
init_error_response (WyreboxDaemonResponseFrame *out_frame,
    const char *request_id, const char *correlation_id, const GError *cause,
    GError **error)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };

  if (!request_id_is_usable (request_id)) {
    g_propagate_error (error, g_error_copy (cause));
    return FALSE;
  }

  if (!wyrebox_daemon_error_frame_init_from_g_error (&error_frame,
          request_id, cause, NULL, error))
    return FALSE;

  return wyrebox_daemon_response_frame_init_error (out_frame,
      &error_frame, correlation_id, error);
}

static gboolean
route_fact_mutation (WyreboxDaemonFactMutationService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (request_frame->fact_mutation == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  return wyrebox_daemon_fact_mutation_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id,
      request_frame->fact_mutation, out_frame, error);
}

gboolean
wyrebox_daemon_request_router_route (WyreboxDaemonFactMutationService
    *fact_mutation_service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE
      (fact_mutation_service), FALSE);
  g_return_val_if_fail (request_frame != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  switch (request_frame->operation) {
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION:
      return route_fact_mutation (fact_mutation_service, request_frame,
          out_frame, error);
    default:
      g_set_error (&local_error,
          G_IO_ERROR,
          G_IO_ERROR_NOT_SUPPORTED, "unsupported daemon request operation");
      return init_error_response (out_frame,
          request_frame->request_id, request_frame->correlation_id,
          local_error, error);
  }
}
