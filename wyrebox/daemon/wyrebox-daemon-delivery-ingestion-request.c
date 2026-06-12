#include "wyrebox-daemon-delivery-ingestion-request.h"

#include <gio/gio.h>

static gboolean
validate_non_empty_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "delivery ingestion %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "delivery ingestion %s must not contain control characters",
          field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_optional_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0')
    return TRUE;

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "delivery ingestion %s must not contain control characters",
          field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_recipients (const gchar *const *recipients, GError **error)
{
  if (recipients == NULL || recipients[0] == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "delivery ingestion recipients are required");
    return FALSE;
  }

  for (guint i = 0; recipients[i] != NULL; i++) {
    const char *recipient = recipients[i];

    if (!validate_non_empty_text (recipient, "recipient", error))
      return FALSE;

  }

  return TRUE;
}

static gboolean
validate_message_bytes (GBytes *message_bytes, GError **error)
{
  if (message_bytes == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "delivery ingestion message bytes are required");
    return FALSE;
  }

  if (g_bytes_get_size (message_bytes) == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "delivery ingestion message bytes must not be empty");
    return FALSE;
  }

  return TRUE;
}

static gboolean
copy_recipients (const gchar *const *recipients,
    gchar ***out_recipients, GError **error)
{
  g_auto (GStrv) copied = NULL;
  gsize count = 0;

  g_return_val_if_fail (out_recipients != NULL, FALSE);

  *out_recipients = NULL;

  if (!validate_recipients (recipients, error))
    return FALSE;

  for (; recipients[count] != NULL; count++);

  copied = g_new0 (gchar *, count + 1);
  for (gsize i = 0; recipients[i] != NULL; i++)
    copied[i] = g_strdup (recipients[i]);

  *out_recipients = g_steal_pointer (&copied);
  return TRUE;
}

void wyrebox_daemon_delivery_ingestion_request_clear
    (WyreboxDaemonDeliveryIngestionRequest * request)
{
  if (request == NULL)
    return;

  g_clear_pointer (&request->delivery_id, g_free);
  g_clear_pointer (&request->queue_id, g_free);
  g_clear_pointer (&request->envelope_sender, g_free);
  g_clear_pointer (&request->recipients, g_strfreev);
  g_clear_pointer (&request->message_bytes, g_bytes_unref);
}

gboolean
    wyrebox_daemon_delivery_ingestion_request_init
    (WyreboxDaemonDeliveryIngestionRequest * request, const char *delivery_id,
    const char *queue_id, const char *envelope_sender,
    const gchar * const *recipients, GBytes * message_bytes, GError ** error)
{
  g_auto (WyreboxDaemonDeliveryIngestionRequest) next = { 0 };
  g_auto (GStrv) copied_recipients = NULL;

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_non_empty_text (delivery_id, "delivery_id", error))
    return FALSE;

  if (!validate_optional_text (queue_id, "queue_id", error))
    return FALSE;

  if (!validate_optional_text (envelope_sender, "envelope_sender", error))
    return FALSE;

  if (!copy_recipients (recipients, &copied_recipients, error))
    return FALSE;

  if (!validate_message_bytes (message_bytes, error))
    return FALSE;

  next.delivery_id = g_strdup (delivery_id);
  next.queue_id = g_strdup (queue_id);
  next.envelope_sender = g_strdup (envelope_sender);
  next.recipients = g_steal_pointer (&copied_recipients);
  next.message_bytes = g_bytes_ref (message_bytes);

  wyrebox_daemon_delivery_ingestion_request_clear (request);
  *request = next;
  memset (&next, 0, sizeof (next));

  return TRUE;
}
