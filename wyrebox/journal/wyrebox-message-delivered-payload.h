#pragma once

#include <glib-object.h>

#include "wyrebox-eml-metadata.h"

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

  /*
   * Caller-supplied internal delivery date as Unix microseconds. Zero means
   * absent/unknown at this boundary; this codec stores but does not parse it.
   */
  guint64 internal_date_unix_us;

  /*
   * Owned nullable parsed EML metadata fields. Values are raw unfolded header
   * values supplied by the caller, not parsed or normalized by this codec.
   */
  char *message_id;
  char *subject;
  char *from;
  char *to;
  char *cc;
  char *bcc;
  char *date;

  /*
   * Number of additional Message-ID headers after the first canonical value.
   */
  guint duplicate_message_id_count;

  /*
   * Owned nullable daemon delivery identity fields. These are present only for
   * v3 payloads produced by daemon-backed delivery ingestion.
   */
  char *delivery_id;
  char *queue_id;
  char *account_identity;
  char *envelope_sender;

  /*
   * Ordered null-terminated recipient list. Vector and strings are owned by
   * the payload and are present only for v3 payloads with delivery identity.
   */
  gchar **recipients;
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
 * @metadata: (nullable): parsed EML metadata to copy into the payload.
 * @internal_date_unix_us: caller-supplied internal delivery date as Unix
 *   microseconds, or zero when absent/unknown.
 *
 * Returns: (transfer full): encoded MessageDelivered journal payload bytes.
 */
GBytes *wyrebox_message_delivered_payload_encode_full (const char *object_key,
    guint64 size_bytes,
    const WyreboxEmlMetadata *metadata,
    guint64 internal_date_unix_us,
    GError **error);

/*
 * @metadata: (nullable): parsed EML metadata to copy into the payload.
 * @internal_date_unix_us: caller-supplied internal delivery date as Unix
 *   microseconds, or zero when absent/unknown.
 * @delivery_id: stable daemon delivery id. Required for identity-aware v3.
 * @queue_id: (nullable): upstream queue id when available.
 * @account_identity: (nullable): daemon account identity when available.
 * @envelope_sender: (nullable): SMTP envelope sender when available.
 * @recipients: (array zero-terminated=1) (nullable): ordered envelope
 *   recipients.
 *
 * Returns: (transfer full): encoded MessageDelivered journal payload bytes.
 */
GBytes *wyrebox_message_delivered_payload_encode_with_identity (
    const char *object_key,
    guint64 size_bytes,
    const WyreboxEmlMetadata *metadata,
    guint64 internal_date_unix_us,
    const char *delivery_id,
    const char *queue_id,
    const char *account_identity,
    const char *envelope_sender,
    const gchar * const *recipients,
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
