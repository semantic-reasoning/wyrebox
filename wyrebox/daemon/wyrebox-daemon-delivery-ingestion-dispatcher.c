#include "wyrebox-daemon-delivery-ingestion-dispatcher.h"

#include "wyrebox-daemon-error-frame.h"

#include <gio/gio.h>

static gboolean
init_error_response_from_cause (WyreboxDaemonResponseFrame *out_frame,
    const char *request_id, const char *correlation_id, const GError *cause,
    GError **error)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_autoptr (GError) fallback_error = NULL;
  const GError *effective_cause = cause;

  if (effective_cause == NULL) {
    g_set_error (&fallback_error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "delivery ingestion dispatcher failed without error detail");
    effective_cause = fallback_error;
  }

  if (!wyrebox_daemon_error_frame_init_from_g_error (&error_frame,
          request_id, effective_cause, NULL, error))
    return FALSE;

  return wyrebox_daemon_response_frame_init_error (out_frame,
      &error_frame, correlation_id, error);
}

gboolean
    wyrebox_daemon_delivery_ingestion_dispatch
    (WyreboxDaemonDeliveryIngestionService * service, const char *request_id,
    const char *caller_identity, const char *account_identity,
    const char *tool_identity, const char *correlation_id,
    const WyreboxDaemonDeliveryIngestionRequest * request,
    WyreboxDaemonResponseFrame * out_frame, GError ** error)
{
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_DELIVERY_INGESTION_SERVICE (service),
      FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_daemon_request_identity_init (&identity,
          request_id,
          caller_identity,
          account_identity, tool_identity, correlation_id, error))
    return FALSE;

  if (wyrebox_daemon_delivery_ingestion_service_handle_identity (service,
          &identity, request, out_frame, &local_error))
    return TRUE;

  return init_error_response_from_cause (out_frame,
      identity.request_id, identity.correlation_id, local_error, error);
}
