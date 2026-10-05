#pragma once

#include "wyrebox-daemon-delivery-ingestion-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"
#include "wyrebox-eml-ingestor.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_DELIVERY_INGESTION_SERVICE \
  (wyrebox_daemon_delivery_ingestion_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonDeliveryIngestionService,
    wyrebox_daemon_delivery_ingestion_service, WYREBOX,
    DAEMON_DELIVERY_INGESTION_SERVICE, GObject)

typedef gboolean (*WyreboxDaemonDeliveryIngestionServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxEmlIngestResult *out_result,
    gpointer user_data,
    GError **error);

WyreboxDaemonDeliveryIngestionService *
wyrebox_daemon_delivery_ingestion_service_new (
    WyreboxDaemonDeliveryIngestionServiceFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

WyreboxDaemonDeliveryIngestionService *
wyrebox_daemon_delivery_ingestion_service_new_with_ingestor (
    WyreboxEmlIngestor *ingestor);

/*
 * Called after a delivery has been durably ingested and its success receipt
 * built, before handle_identity() returns its success frame. Runs on the
 * thread handling the request and cannot fail the delivery.
 *
 * @result: (transfer none): the ingest result, valid for the call only.
 */
typedef void (*WyreboxDaemonDeliveryIngestionServicePostIngestFunc) (
    const WyreboxEmlIngestResult *result,
    gpointer user_data);

/*
 * Installs @func as the post-ingest hook, replacing and destroying any
 * previous hook data. Call before the service handles requests.
 *
 * @user_data: (transfer full): released with @user_data_destroy when the hook
 *   is replaced or the service is finalized.
 */
void wyrebox_daemon_delivery_ingestion_service_set_post_ingest_hook (
    WyreboxDaemonDeliveryIngestionService *self,
    WyreboxDaemonDeliveryIngestionServicePostIngestFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

/*
 * Rejects callers other than "postfix" with G_IO_ERROR_PERMISSION_DENIED, and
 * requests without a non-empty account identity with
 * G_IO_ERROR_INVALID_ARGUMENT, before anything is ingested.
 */
gboolean wyrebox_daemon_delivery_ingestion_service_handle_identity (
    WyreboxDaemonDeliveryIngestionService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
