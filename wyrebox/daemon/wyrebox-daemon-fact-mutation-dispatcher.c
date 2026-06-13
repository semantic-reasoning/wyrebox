#include "wyrebox-daemon-fact-mutation-dispatcher.h"

#include "wyrebox-daemon-error-frame.h"

#include <gio/gio.h>

static gboolean
init_error_response_from_cause (WyreboxDaemonResponseFrame *out_frame,
    const char *request_id, const char *correlation_id, const GError *cause,
    GError **error)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };

  if (!wyrebox_daemon_error_frame_init_from_g_error (&error_frame,
          request_id, cause, NULL, error))
    return FALSE;

  return wyrebox_daemon_response_frame_init_error (out_frame,
      &error_frame, correlation_id, error);
}

gboolean
wyrebox_daemon_fact_mutation_dispatch (WyreboxDaemonFactMutationService
    *service, const char *request_id, const char *caller_identity,
    const char *account_identity, const char *tool_identity,
    const char *correlation_id, const WyreboxDaemonFactMutationRequest *request,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (service),
      FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_daemon_request_identity_init (&identity,
          request_id,
          caller_identity,
          account_identity, tool_identity, correlation_id, error))
    return FALSE;

  if (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, request, out_frame, &local_error))
    return TRUE;

  return init_error_response_from_cause (out_frame,
      identity.request_id, identity.correlation_id, local_error, error);
}

gboolean
wyrebox_daemon_fact_batch_import_dispatch (WyreboxDaemonFactMutationService
    *service, const char *request_id, const char *caller_identity,
    const char *account_identity, const char *tool_identity,
    const char *correlation_id,
    const WyreboxDaemonFactBatchImportRequest *request,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (service),
      FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_daemon_request_identity_init (&identity,
          request_id,
          caller_identity,
          account_identity, tool_identity, correlation_id, error))
    return FALSE;

  if (wyrebox_daemon_fact_mutation_service_handle_batch_identity (service,
          &identity, request, out_frame, &local_error))
    return TRUE;

  return init_error_response_from_cause (out_frame,
      identity.request_id, identity.correlation_id, local_error, error);
}
