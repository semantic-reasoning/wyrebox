#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>

#define WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC "WYREMDP1"
#define WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_LEN 8
#define WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE 18
#define WYREBOX_SHA256_OBJECT_KEY_PREFIX "sha256:"
#define WYREBOX_SHA256_OBJECT_KEY_PREFIX_LEN 7
#define WYREBOX_SHA256_HEX_LEN 64
#define WYREBOX_SHA256_OBJECT_KEY_LEN \
  (WYREBOX_SHA256_OBJECT_KEY_PREFIX_LEN + WYREBOX_SHA256_HEX_LEN)

/*
 * MessageDelivered payload v1 binary format:
 *
 *   0..7    ASCII magic "WYREMDP1"
 *   8..15   size_bytes, little-endian guint64
 *   16..17  object_key byte length, little-endian guint16
 *   18..N   object_key bytes, no NUL terminator
 *
 * The v1 decoder currently accepts only sha256:<64 lowercase hex> object keys
 * and rejects trailing bytes by requiring N == 18 + object_key_len.
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

static inline guint16
read_u16_le (const guint8 *buffer)
{
  return (guint16) (buffer[0] | ((guint16) buffer[1] << 8));
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

void
wyrebox_message_delivered_payload_clear (WyreboxMessageDeliveredPayload
    *payload)
{
  if (payload == NULL)
    return;

  g_clear_pointer (&payload->object_key, g_free);
  payload->size_bytes = 0;
}

GBytes *
wyrebox_message_delivered_payload_encode (const char *object_key,
    guint64 size_bytes, GError **error)
{
  g_autofree guint8 *data = NULL;
  gsize object_key_len = 0;
  gsize payload_len = 0;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!validate_object_key (object_key, G_IO_ERROR_INVALID_ARGUMENT, error))
    return NULL;

  object_key_len = strlen (object_key);
  payload_len = WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE + object_key_len;
  data = g_malloc0 (payload_len);

  memcpy (data,
      WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC,
      WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_LEN);
  write_u64_le (data + 8, size_bytes);
  write_u16_le (data + 16, (guint16) object_key_len);
  memcpy (data + WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE,
      object_key, object_key_len);

  return g_bytes_new_take (g_steal_pointer (&data), payload_len);
}

gboolean
wyrebox_message_delivered_payload_decode (GBytes *bytes,
    WyreboxMessageDeliveredPayload *out_payload, GError **error)
{
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };
  const guint8 *data = NULL;
  gsize size = 0;
  guint16 object_key_len = 0;
  guint64 size_bytes = 0;

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

  if (size < WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "MessageDelivered payload is truncated");
    return FALSE;
  }

  if (memcmp (data,
          WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC,
          WYREBOX_MESSAGE_DELIVERED_PAYLOAD_MAGIC_LEN) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "invalid MessageDelivered payload magic");
    return FALSE;
  }

  size_bytes = read_u64_le (data + 8);
  object_key_len = read_u16_le (data + 16);
  if (object_key_len == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "MessageDelivered payload object key is empty");
    return FALSE;
  }

  if (size != WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE +
      (gsize) object_key_len) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "MessageDelivered payload length is malformed");
    return FALSE;
  }

  decoded.object_key = g_strndup (
      (const char *) data + WYREBOX_MESSAGE_DELIVERED_PAYLOAD_HEADER_SIZE,
      object_key_len);
  decoded.size_bytes = size_bytes;

  if (!validate_object_key (decoded.object_key, G_IO_ERROR_INVALID_DATA, error)) {
    g_prefix_error (error, "invalid MessageDelivered payload: ");
    return FALSE;
  }

  *out_payload = decoded;
  decoded.object_key = NULL;
  decoded.size_bytes = 0;

  return TRUE;
}
