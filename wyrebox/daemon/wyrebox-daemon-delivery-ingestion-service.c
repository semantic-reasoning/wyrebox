#include "wyrebox-daemon-delivery-ingestion-service.h"
#include "wyrebox-daemon-success-receipt.h"

#include <gio/gio.h>

struct _WyreboxDaemonDeliveryIngestionService
{
  GObject parent_instance;

  WyreboxDaemonDeliveryIngestionServiceFunc func;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
};

G_DEFINE_TYPE (WyreboxDaemonDeliveryIngestionService,
    wyrebox_daemon_delivery_ingestion_service, G_TYPE_OBJECT);

static gboolean
caller_can_ingest_messages (const char *caller_identity)
{
  return g_strcmp0 (caller_identity, "postfix") == 0;
}

static gboolean
authorize_delivery_ingestion_identity (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonDeliveryIngestionRequest *request,
    GError **error)
{
  (void) request;

  if (!caller_can_ingest_messages (identity->caller_identity)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized to ingest deliveries");
    return FALSE;
  }

  return TRUE;
}

static void
wyrebox_daemon_delivery_ingestion_service_finalize (GObject *object)
{
  WyreboxDaemonDeliveryIngestionService *self =
      WYREBOX_DAEMON_DELIVERY_INGESTION_SERVICE (object);

  if (self->user_data_destroy != NULL && self->user_data != NULL)
    self->user_data_destroy (self->user_data);

  G_OBJECT_CLASS
      (wyrebox_daemon_delivery_ingestion_service_parent_class)->finalize
      (object);
}

static void
    wyrebox_daemon_delivery_ingestion_service_class_init
    (WyreboxDaemonDeliveryIngestionServiceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_delivery_ingestion_service_finalize;
}

static void
    wyrebox_daemon_delivery_ingestion_service_init
    (WyreboxDaemonDeliveryIngestionService * self)
{
  (void) self;
}

WyreboxDaemonDeliveryIngestionService
    * wyrebox_daemon_delivery_ingestion_service_new
    (WyreboxDaemonDeliveryIngestionServiceFunc func, gpointer user_data,
    GDestroyNotify user_data_destroy) {
  g_return_val_if_fail (func != NULL, NULL);

  WyreboxDaemonDeliveryIngestionService *self =
      g_object_new (WYREBOX_TYPE_DAEMON_DELIVERY_INGESTION_SERVICE, NULL);

  self->func = func;
  self->user_data = user_data;
  self->user_data_destroy = user_data_destroy;

  return self;
}

static gboolean
ingest_delivery_with_ingestor (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxEmlIngestResult *out_result, gpointer user_data, GError **error)
{
  WyreboxEmlIngestor *ingestor = user_data;

  (void) identity;

  g_return_val_if_fail (WYREBOX_IS_EML_INGESTOR (ingestor), FALSE);
  g_return_val_if_fail (request != NULL, FALSE);

  return wyrebox_eml_ingestor_ingest_bytes (ingestor,
      request->message_bytes, out_result, error);
}

WyreboxDaemonDeliveryIngestionService *
wyrebox_daemon_delivery_ingestion_service_new_with_ingestor (WyreboxEmlIngestor
    *ingestor)
{
  g_autoptr (WyreboxEmlIngestor) owned_ingestor = NULL;

  g_return_val_if_fail (WYREBOX_IS_EML_INGESTOR (ingestor), NULL);

  owned_ingestor = g_object_ref (ingestor);

  return
      wyrebox_daemon_delivery_ingestion_service_new
      (ingest_delivery_with_ingestor, g_steal_pointer (&owned_ingestor),
      g_object_unref);
}

gboolean
    wyrebox_daemon_delivery_ingestion_service_handle_identity
    (WyreboxDaemonDeliveryIngestionService * self,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonDeliveryIngestionRequest * request,
    WyreboxDaemonResponseFrame * out_frame, GError ** error)
{
  g_auto (WyreboxDaemonSuccessReceipt) receipt = { 0 };
  g_auto (WyreboxEmlIngestResult) ingest_result = { 0 };
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_DELIVERY_INGESTION_SERVICE (self),
      FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!authorize_delivery_ingestion_identity (identity, request, error))
    return FALSE;

  if (!self->func (identity, request, &ingest_result, self->user_data,
          &local_error)) {
    if (local_error == NULL) {
      g_set_error (&local_error,
          G_IO_ERROR,
          G_IO_ERROR_FAILED,
          "delivery ingestion service failed without error detail");
    }

    g_propagate_error (error, g_steal_pointer (&local_error));
    return FALSE;
  }

  if (!wyrebox_daemon_success_receipt_init_delivery_ingestion (&receipt,
          identity->request_id, &ingest_result, &local_error)) {
    g_propagate_error (error, g_steal_pointer (&local_error));
    return FALSE;
  }

  return wyrebox_daemon_response_frame_init_success (out_frame,
      &receipt, identity->correlation_id, error);
}
