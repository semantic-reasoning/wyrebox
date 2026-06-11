#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>

#define WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_V1 "WYREMDP1"
#define WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_V2 "WYREMDP2"
#define WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_LEN 8
#define WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE_V1 18
#define WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE_V2 30
#define WYREBOX_MESSAGE_DELIVERED_PAYLOAD_NULL_STRING G_MAXUINT32
#define WYREBOX_SHA256_OBJECT_KEY_PREFIX "sha256:"
#define WYREBOX_SHA256_OBJECT_KEY_PREFIX_LEN 7
#define WYREBOX_SHA256_HEX_LEN 64
#define WYREBOX_SHA256_OBJECT_KEY_LEN \
  (WYREBOX_SHA256_OBJECT_KEY_PREFIX_LEN + WYREBOX_SHA256_HEX_LEN)

/*
 * MessageDelivered payload binary formats:
 *
 * v1 is decoded for journal replay compatibility, but new payloads are always
 * encoded as v2.
 *
 * v1:
 *
 *   0..7    ASCII magic "WYREMDP1"
 *   8..15   size_bytes, little-endian guint64
 *   16..17  object_key byte length, little-endian guint16
 *   18..N   object_key bytes, no NUL terminator
 *
 * v2:
 *
 *   0..7    ASCII magic "WYREMDP2"
 *   8..15   size_bytes, little-endian guint64
 *   16..23  internal_date_unix_us, little-endian guint64; zero is absent
 *   24..27  duplicate_message_id_count, little-endian guint32
 *   28..29  object_key byte length, little-endian guint16
 *   30..N   object_key bytes, no NUL terminator
 *   N..     metadata strings in this order:
 *           message_id, subject, from, to, cc, bcc, date
 *
 * Each metadata string is encoded as a little-endian guint32 byte length
 * followed by that many bytes, without a NUL terminator. G_MAXUINT32 denotes
 * NULL and is not followed by bytes.
 *
 * The decoder accepts only sha256:<64 lowercase hex> object keys and rejects
 * truncated, malformed, and trailing payload bytes.
 */

static inline void
write_u16_le (guint8 *dst, guint16 value)
{
  dst[0] = (guint8) ((value >> 0) & 0xFF);
  dst[1] = (guint8) ((value >> 8) & 0xFF);
}

static inline void
write_u64_le (guint8 *dst, guint64 value)
{
  dst[0] = (guint8) ((value >> 0) & 0xFF);
  dst[1] = (guint8) ((value >> 8) & 0xFF);
  dst[2] = (guint8) ((value >> 16) & 0xFF);
  dst[3] = (guint8) ((value >> 24) & 0xFF);
  dst[4] = (guint8) ((value >> 32) & 0xFF);
  dst[5] = (guint8) ((value >> 40) & 0xFF);
  dst[6] = (guint8) ((value >> 48) & 0xFF);
  dst[7] = (guint8) ((value >> 56) & 0xFF);
}

static inline void
write_u32_le (guint8 *dst, guint32 value)
{
  dst[0] = (guint8) ((value >> 0) & 0xFF);
  dst[1] = (guint8) ((value >> 8) & 0xFF);
  dst[2] = (guint8) ((value >> 16) & 0xFF);
  dst[3] = (guint8) ((value >> 24) & 0xFF);
}

static inline guint16
read_u16_le (const guint8 *buffer)
{
  return (guint16) (buffer[0] | ((guint16) buffer[1] << 8));
}

static inline guint32
read_u32_le (const guint8 *buffer)
{
  return (guint32) buffer[0] |
      ((guint32) buffer[1] << 8) |
      ((guint32) buffer[2] << 16) | ((guint32) buffer[3] << 24);
}

static inline guint64
read_u64_le (const guint8 *buffer)
{
  return (guint64) buffer[0] |
      ((guint64) buffer[1] << 8) |
      ((guint64) buffer[2] << 16) |
      ((guint64) buffer[3] << 24) |
      ((guint64) buffer[4] << 32) |
      ((guint64) buffer[5] << 40) |
      ((guint64) buffer[6] << 48) | ((guint64) buffer[7] << 56);
}

static gboolean
checked_add_size (gsize *total, gsize value, GError **error)
{
  if (value > G_MAXSIZE - *total) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "MessageDelivered payload length overflows addressable memory");
    return FALSE;
  }

  *total += value;
  return TRUE;
}

static gboolean
checked_add_encoded_string_len (gsize *total, const char *value, GError **error)
{
  gsize value_len = 0;

  if (!checked_add_size (total, sizeof (guint32), error))
    return FALSE;

  if (value == NULL)
    return TRUE;

  value_len = strlen (value);
  if (value_len >= WYREBOX_MESSAGE_DELIVERED_PAYLOAD_NULL_STRING) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "MessageDelivered metadata string is too large");
    return FALSE;
  }

  return checked_add_size (total, value_len, error);
}

static void
write_nullable_string (guint8 **cursor, const char *value)
{
  gsize value_len = 0;

  if (value == NULL) {
    write_u32_le (*cursor, WYREBOX_MESSAGE_DELIVERED_PAYLOAD_NULL_STRING);
    *cursor += sizeof (guint32);
    return;
  }

  value_len = strlen (value);
  g_assert (value_len < WYREBOX_MESSAGE_DELIVERED_PAYLOAD_NULL_STRING);
  write_u32_le (*cursor, (guint32) value_len);
  *cursor += sizeof (guint32);
  memcpy (*cursor, value, value_len);
  *cursor += value_len;
}

static gboolean
read_nullable_string (const guint8 *data,
    gsize size, gsize *offset, char **out_value, GError **error)
{
  guint32 value_len = 0;

  g_assert (out_value != NULL);
  *out_value = NULL;

  if (size - *offset < sizeof (guint32)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "MessageDelivered payload is truncated");
    return FALSE;
  }

  value_len = read_u32_le (data + *offset);
  *offset += sizeof (guint32);

  if (value_len == WYREBOX_MESSAGE_DELIVERED_PAYLOAD_NULL_STRING)
    return TRUE;

  if ((gsize) value_len > size - *offset) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "MessageDelivered payload is truncated");
    return FALSE;
  }

  *out_value = g_strndup ((const char *) data + *offset, value_len);
  *offset += value_len;

  return TRUE;
}

static void
take_decoded_payload (WyreboxMessageDeliveredPayload *out_payload,
    WyreboxMessageDeliveredPayload *decoded)
{
  *out_payload = *decoded;
  memset (decoded, 0, sizeof (*decoded));
}

static gboolean
is_lowercase_hex (char value)
{
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

static void
set_invalid_object_key_error (GError **error, GIOErrorEnum code)
{
  g_set_error (error,
      G_IO_ERROR,
      code,
      "object key must be sha256:%u lowercase hex characters",
      WYREBOX_SHA256_HEX_LEN);
}

static gboolean
validate_object_key (const char *object_key, GIOErrorEnum code, GError **error)
{
  gsize len = 0;

  if (object_key == NULL) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "object key is required");
    return FALSE;
  }

  len = strlen (object_key);
  if (len != WYREBOX_SHA256_OBJECT_KEY_LEN ||
      !g_str_has_prefix (object_key, WYREBOX_SHA256_OBJECT_KEY_PREFIX)) {
    set_invalid_object_key_error (error, code);
    return FALSE;
  }

  for (gsize index = WYREBOX_SHA256_OBJECT_KEY_PREFIX_LEN; index < len; index++) {
    if (!is_lowercase_hex (object_key[index])) {
      set_invalid_object_key_error (error, code);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
decode_v1_payload (const guint8 *data,
    gsize size, WyreboxMessageDeliveredPayload *out_payload, GError **error)
{
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };
  guint16 object_key_len = 0;
  gsize expected_size = 0;

  if (size < WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE_V1) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "MessageDelivered payload is truncated");
    return FALSE;
  }

  object_key_len = read_u16_le (data + 16);
  if (object_key_len == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "MessageDelivered payload object key is empty");
    return FALSE;
  }

  expected_size = WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE_V1 +
      (gsize) object_key_len;
  if (expected_size != size) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        expected_size > size ?
        "MessageDelivered payload is truncated" :
        "MessageDelivered payload length is malformed");
    return FALSE;
  }

  decoded.object_key = g_strndup ((const char *) data +
      WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE_V1, object_key_len);
  decoded.size_bytes = read_u64_le (data + 8);

  if (!validate_object_key (decoded.object_key, G_IO_ERROR_INVALID_DATA, error)) {
    g_prefix_error (error, "invalid MessageDelivered payload: ");
    return FALSE;
  }

  take_decoded_payload (out_payload, &decoded);
  return TRUE;
}

static gboolean
decode_v2_payload (const guint8 *data,
    gsize size, WyreboxMessageDeliveredPayload *out_payload, GError **error)
{
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };
  gsize offset = 0;
  guint16 object_key_len = 0;
  guint64 size_bytes = 0;
  guint64 internal_date_unix_us = 0;
  guint32 duplicate_message_id_count = 0;

  if (size < WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE_V2) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "MessageDelivered payload is truncated");
    return FALSE;
  }

  size_bytes = read_u64_le (data + 8);
  internal_date_unix_us = read_u64_le (data + 16);
  duplicate_message_id_count = read_u32_le (data + 24);
  object_key_len = read_u16_le (data + 28);
  if (object_key_len == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "MessageDelivered payload object key is empty");
    return FALSE;
  }

  offset = WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE_V2;
  if ((gsize) object_key_len > size - offset) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "MessageDelivered payload is truncated");
    return FALSE;
  }

  decoded.object_key = g_strndup ((const char *) data + offset, object_key_len);
  offset += object_key_len;
  decoded.size_bytes = size_bytes;
  decoded.internal_date_unix_us = internal_date_unix_us;
  decoded.duplicate_message_id_count = duplicate_message_id_count;

  if (!validate_object_key (decoded.object_key, G_IO_ERROR_INVALID_DATA, error)) {
    g_prefix_error (error, "invalid MessageDelivered payload: ");
    return FALSE;
  }

  if (!read_nullable_string (data, size, &offset, &decoded.message_id, error) ||
      !read_nullable_string (data, size, &offset, &decoded.subject, error) ||
      !read_nullable_string (data, size, &offset, &decoded.from, error) ||
      !read_nullable_string (data, size, &offset, &decoded.to, error) ||
      !read_nullable_string (data, size, &offset, &decoded.cc, error) ||
      !read_nullable_string (data, size, &offset, &decoded.bcc, error) ||
      !read_nullable_string (data, size, &offset, &decoded.date, error))
    return FALSE;

  if (offset != size) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "MessageDelivered payload length is malformed");
    return FALSE;
  }

  take_decoded_payload (out_payload, &decoded);
  return TRUE;
}

void
wyrebox_message_delivered_payload_clear (WyreboxMessageDeliveredPayload
    *payload)
{
  if (payload == NULL)
    return;

  g_clear_pointer (&payload->object_key, g_free);
  g_clear_pointer (&payload->message_id, g_free);
  g_clear_pointer (&payload->subject, g_free);
  g_clear_pointer (&payload->from, g_free);
  g_clear_pointer (&payload->to, g_free);
  g_clear_pointer (&payload->cc, g_free);
  g_clear_pointer (&payload->bcc, g_free);
  g_clear_pointer (&payload->date, g_free);
  payload->size_bytes = 0;
  payload->internal_date_unix_us = 0;
  payload->duplicate_message_id_count = 0;
}

GBytes *
wyrebox_message_delivered_payload_encode (const char *object_key,
    guint64 size_bytes, GError **error)
{
  return wyrebox_message_delivered_payload_encode_full (object_key,
      size_bytes, NULL, 0, error);
}

GBytes *
wyrebox_message_delivered_payload_encode_full (const char *object_key,
    guint64 size_bytes, const WyreboxEmlMetadata *metadata,
    guint64 internal_date_unix_us, GError **error)
{
  g_autofree guint8 *data = NULL;
  guint8 *cursor = NULL;
  gsize object_key_len = 0;
  gsize payload_len = 0;
  const char *message_id = NULL;
  const char *subject = NULL;
  const char *from = NULL;
  const char *to = NULL;
  const char *cc = NULL;
  const char *bcc = NULL;
  const char *date = NULL;
  guint duplicate_message_id_count = 0;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!validate_object_key (object_key, G_IO_ERROR_INVALID_ARGUMENT, error))
    return NULL;

  if (metadata != NULL) {
    message_id = metadata->message_id;
    subject = metadata->subject;
    from = metadata->from;
    to = metadata->to;
    cc = metadata->cc;
    bcc = metadata->bcc;
    date = metadata->date;
    duplicate_message_id_count = metadata->duplicate_message_id_count;
  }

  object_key_len = strlen (object_key);
  payload_len = WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE_V2;
  if (!checked_add_size (&payload_len, object_key_len, error) ||
      !checked_add_encoded_string_len (&payload_len, message_id, error) ||
      !checked_add_encoded_string_len (&payload_len, subject, error) ||
      !checked_add_encoded_string_len (&payload_len, from, error) ||
      !checked_add_encoded_string_len (&payload_len, to, error) ||
      !checked_add_encoded_string_len (&payload_len, cc, error) ||
      !checked_add_encoded_string_len (&payload_len, bcc, error) ||
      !checked_add_encoded_string_len (&payload_len, date, error))
    return NULL;

  data = g_malloc0 (payload_len);

  memcpy (data,
      WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_V2,
      WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_LEN);
  write_u64_le (data + 8, size_bytes);
  write_u64_le (data + 16, internal_date_unix_us);
  write_u32_le (data + 24, duplicate_message_id_count);
  write_u16_le (data + 28, (guint16) object_key_len);
  memcpy (data + WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE_V2,
      object_key, object_key_len);
  cursor = data + WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE_V2 +
      object_key_len;
  write_nullable_string (&cursor, message_id);
  write_nullable_string (&cursor, subject);
  write_nullable_string (&cursor, from);
  write_nullable_string (&cursor, to);
  write_nullable_string (&cursor, cc);
  write_nullable_string (&cursor, bcc);
  write_nullable_string (&cursor, date);
  g_assert (cursor == data + payload_len);

  return g_bytes_new_take (g_steal_pointer (&data), payload_len);
}

gboolean
wyrebox_message_delivered_payload_decode (GBytes *bytes,
    WyreboxMessageDeliveredPayload *out_payload, GError **error)
{
  const guint8 *data = NULL;
  gsize size = 0;

  g_return_val_if_fail (bytes != NULL, FALSE);
  g_return_val_if_fail (out_payload != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  data = g_bytes_get_data (bytes, &size);
  if (size == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "MessageDelivered payload is empty");
    return FALSE;
  }

  if (size < WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_LEN) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "MessageDelivered payload is truncated");
    return FALSE;
  }

  if (memcmp (data,
          WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_V1,
          WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_LEN) == 0)
    return decode_v1_payload (data, size, out_payload, error);

  if (memcmp (data,
          WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_V2,
          WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_LEN) == 0)
    return decode_v2_payload (data, size, out_payload, error);

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA, "invalid MessageDelivered payload magic");
  return FALSE;
}
