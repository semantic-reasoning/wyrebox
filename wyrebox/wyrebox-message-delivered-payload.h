#pragma once

#include <glib-object.h>

typedef struct
{
  /*
   * Object key for the immutable raw RFC 5322 bytes.
   *
   * Ownership: caller owns and must free with g_free() or
   * wyrebox_message_delivered_payload_clear().
   */
  char *object_key;

  /*
   * Size of the delivered raw RFC 5322 bytes, in bytes.
   */
  guint64 size_bytes;
} WyreboxMessageDeliveredPayload;

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Clears owned fields in @payload and leaves it reusable as an empty payload.
 */
void wyrebox_message_delivered_payload_clear (
    WyreboxMessageDeliveredPayload *payload);

/*
 * @object_key: immutable raw object key, currently sha256:<64 lowercase hex>.
 *
 * Returns: (transfer full): encoded MessageDelivered journal payload bytes.
 */
GBytes *wyrebox_message_delivered_payload_encode (const char *object_key,
    guint64 size_bytes,
    GError **error);

/*
 * @bytes: (transfer none): encoded MessageDelivered journal payload bytes.
 * @out_payload: (out): receives decoded owned fields. The caller owns fields in
 *   the payload and must clear it with
 *   wyrebox_message_delivered_payload_clear().
 */
gboolean wyrebox_message_delivered_payload_decode (GBytes *bytes,
    WyreboxMessageDeliveredPayload *out_payload,
    GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxMessageDeliveredPayload,
    wyrebox_message_delivered_payload_clear)

G_END_DECLS
/* *INDENT-ON* */
