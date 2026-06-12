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

gboolean wyrebox_daemon_delivery_ingestion_service_handle_identity (
    WyreboxDaemonDeliveryIngestionService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
