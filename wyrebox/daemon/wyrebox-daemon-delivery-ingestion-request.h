#pragma once

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Strings are owned by the request and released by clear().
   */
  char *delivery_id;
  char *queue_id;
  char *envelope_sender;

  /*
   * Null-terminated list of recipients. Vector is owned by the request.
   */
  gchar **recipients;

  /*
   * Reference-counted message bytes.
   */
  GBytes *message_bytes;
} WyreboxDaemonDeliveryIngestionRequest;

void wyrebox_daemon_delivery_ingestion_request_clear (
    WyreboxDaemonDeliveryIngestionRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonDeliveryIngestionRequest,
    wyrebox_daemon_delivery_ingestion_request_clear)

gboolean wyrebox_daemon_delivery_ingestion_request_init (
    WyreboxDaemonDeliveryIngestionRequest *request,
    const char *delivery_id,
    const char *queue_id,
    const char *envelope_sender,
    const gchar * const *recipients,
    GBytes *message_bytes,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
