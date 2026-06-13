#include "wyrebox-derived-view-membership-changed-payload.h"

#include <gio/gio.h>
#include <string.h>

#define WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_MAGIC "WYREDVC1"
#define WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_MAGIC_LEN 8
#define WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_HEADER_SIZE 33

static inline void
write_u32_le (guint8 *dst, guint32 value)
{
  dst[0] = (guint8) ((value >> 0) & 0xff);
  dst[1] = (guint8) ((value >> 8) & 0xff);
  dst[2] = (guint8) ((value >> 16) & 0xff);
  dst[3] = (guint8) ((value >> 24) & 0xff);
}

static inline void
write_u64_le (guint8 *dst, guint64 value)
{
  dst[0] = (guint8) ((value >> 0) & 0xff);
  dst[1] = (guint8) ((value >> 8) & 0xff);
  dst[2] = (guint8) ((value >> 16) & 0xff);
  dst[3] = (guint8) ((value >> 24) & 0xff);
  dst[4] = (guint8) ((value >> 32) & 0xff);
  dst[5] = (guint8) ((value >> 40) & 0xff);
  dst[6] = (guint8) ((value >> 48) & 0xff);
  dst[7] = (guint8) ((value >> 56) & 0xff);
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
        "DerivedViewMembershipChanged payload length overflows memory");
    return FALSE;
  }

  *total += value;
  return TRUE;
}

static gboolean
validate_required_string (const char *field_name, const char *value,
    GIOErrorEnum code, GError **error)
{
  if (value == NULL || value[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        code,
        "DerivedViewMembershipChanged payload %s is required", field_name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
checked_add_string (gsize *total, const char *field_name, const char *value,
    GError **error)
{
  gsize len = 0;

  if (!validate_required_string (field_name, value,
          G_IO_ERROR_INVALID_ARGUMENT, error))
    return FALSE;

  len = strlen (value);
  if (len > G_MAXUINT32) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "DerivedViewMembershipChanged payload string is too large");
    return FALSE;
  }

  return checked_add_size (total, sizeof (guint32), error) &&
      checked_add_size (total, len, error);
}

static void
write_string (guint8 **cursor, const char *value)
{
  gsize len = strlen (value);

  g_assert (len <= G_MAXUINT32);
  write_u32_le (*cursor, (guint32) len);
  *cursor += sizeof (guint32);
  memcpy (*cursor, value, len);
  *cursor += len;
}

static gboolean
read_string (const guint8 *data,
    gsize size, gsize *offset, char **out_value, GError **error)
{
  guint32 len = 0;

  g_assert (out_value != NULL);
  *out_value = NULL;

  if (size - *offset < sizeof (guint32)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DerivedViewMembershipChanged payload is truncated");
    return FALSE;
  }

  len = read_u32_le (data + *offset);
  *offset += sizeof (guint32);
  if ((gsize) len > size - *offset) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DerivedViewMembershipChanged payload is truncated");
    return FALSE;
  }

  if (len == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DerivedViewMembershipChanged payload string is empty");
    return FALSE;
  }

  if (memchr (data + *offset, '\0', len) != NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DerivedViewMembershipChanged payload string contains embedded NUL");
    return FALSE;
  }

  *out_value = g_strndup ((const char *) data + *offset, len);
  *offset += len;
  return TRUE;
}

static gboolean
validate_payload (const WyreboxDerivedViewMembershipChangedPayload *payload,
    GIOErrorEnum code, GError **error)
{
  if (payload == NULL) {
    g_set_error (error,
        G_IO_ERROR, code, "DerivedViewMembershipChanged payload is required");
    return FALSE;
  }

  if (!validate_required_string ("account_id", payload->account_id, code,
          error) ||
      !validate_required_string ("view_id", payload->view_id, code, error) ||
      !validate_required_string ("message_id", payload->message_id, code,
          error) ||
      !validate_required_string ("membership_id", payload->membership_id,
          code, error) ||
      !validate_required_string ("rule_version_hash",
          payload->rule_version_hash, code, error))
    return FALSE;

  if (payload->uid == 0) {
    g_set_error (error,
        G_IO_ERROR,
        code, "DerivedViewMembershipChanged payload uid is required");
    return FALSE;
  }

  if (payload->uidvalidity == 0) {
    g_set_error (error,
        G_IO_ERROR,
        code, "DerivedViewMembershipChanged payload uidvalidity is required");
    return FALSE;
  }

  if (payload->materialized_at_unix_us == 0) {
    g_set_error (error,
        G_IO_ERROR,
        code,
        "DerivedViewMembershipChanged payload materialization timestamp is required");
    return FALSE;
  }

  return TRUE;
}

static void
take_decoded_payload (WyreboxDerivedViewMembershipChangedPayload *out_payload,
    WyreboxDerivedViewMembershipChangedPayload *decoded)
{
  *out_payload = *decoded;
  memset (decoded, 0, sizeof (*decoded));
}

void wyrebox_derived_view_membership_changed_payload_clear
    (WyreboxDerivedViewMembershipChangedPayload * payload)
{
  if (payload == NULL)
    return;

  g_clear_pointer (&payload->account_id, g_free);
  g_clear_pointer (&payload->view_id, g_free);
  g_clear_pointer (&payload->message_id, g_free);
  g_clear_pointer (&payload->membership_id, g_free);
  g_clear_pointer (&payload->rule_version_hash, g_free);
  payload->uid = 0;
  payload->uidvalidity = 0;
  payload->is_visible = FALSE;
  payload->materialized_at_unix_us = 0;
}

GBytes *wyrebox_derived_view_membership_changed_payload_encode
    (const WyreboxDerivedViewMembershipChangedPayload * payload,
    GError ** error)
{
  g_autofree guint8 *data = NULL;
  guint8 *cursor = NULL;
  gsize payload_len =
      WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_HEADER_SIZE;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!validate_payload (payload, G_IO_ERROR_INVALID_ARGUMENT, error) ||
      !checked_add_string (&payload_len, "account_id", payload->account_id,
          error) ||
      !checked_add_string (&payload_len, "view_id", payload->view_id, error) ||
      !checked_add_string (&payload_len, "message_id", payload->message_id,
          error) ||
      !checked_add_string (&payload_len, "membership_id",
          payload->membership_id, error) ||
      !checked_add_string (&payload_len, "rule_version_hash",
          payload->rule_version_hash, error))
    return NULL;

  data = g_malloc0 (payload_len);
  memcpy (data, WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_MAGIC,
      WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_MAGIC_LEN);
  write_u64_le (data + 8, payload->uid);
  write_u64_le (data + 16, payload->uidvalidity);
  data[24] = payload->is_visible ? 1 : 0;
  write_u64_le (data + 25, payload->materialized_at_unix_us);

  cursor = data + WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_HEADER_SIZE;
  write_string (&cursor, payload->account_id);
  write_string (&cursor, payload->view_id);
  write_string (&cursor, payload->message_id);
  write_string (&cursor, payload->membership_id);
  write_string (&cursor, payload->rule_version_hash);
  g_assert (cursor == data + payload_len);

  return g_bytes_new_take (g_steal_pointer (&data), payload_len);
}

gboolean
wyrebox_derived_view_membership_changed_payload_decode (GBytes *bytes,
    WyreboxDerivedViewMembershipChangedPayload *out_payload, GError **error)
{
  const guint8 *data = NULL;
  gsize size = 0;
  gsize offset = WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_HEADER_SIZE;
  g_auto (WyreboxDerivedViewMembershipChangedPayload) decoded = { 0 };

  g_return_val_if_fail (bytes != NULL, FALSE);
  g_return_val_if_fail (out_payload != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  data = g_bytes_get_data (bytes, &size);
  if (size < WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_MAGIC_LEN) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DerivedViewMembershipChanged payload is truncated");
    return FALSE;
  }

  if (memcmp (data,
          WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_MAGIC,
          WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_MAGIC_LEN) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "invalid DerivedViewMembershipChanged payload magic");
    return FALSE;
  }

  if (size < WYREBOX_DERIVED_VIEW_MEMBERSHIP_CHANGED_PAYLOAD_HEADER_SIZE) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DerivedViewMembershipChanged payload is truncated");
    return FALSE;
  }

  decoded.uid = read_u64_le (data + 8);
  decoded.uidvalidity = read_u64_le (data + 16);
  if (data[24] > 1) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DerivedViewMembershipChanged payload visibility is malformed");
    return FALSE;
  }
  decoded.is_visible = data[24] == 1;
  decoded.materialized_at_unix_us = read_u64_le (data + 25);

  if (!read_string (data, size, &offset, &decoded.account_id, error) ||
      !read_string (data, size, &offset, &decoded.view_id, error) ||
      !read_string (data, size, &offset, &decoded.message_id, error) ||
      !read_string (data, size, &offset, &decoded.membership_id, error) ||
      !read_string (data, size, &offset, &decoded.rule_version_hash, error))
    return FALSE;

  if (offset != size) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DerivedViewMembershipChanged payload length is malformed");
    return FALSE;
  }

  if (!validate_payload (&decoded, G_IO_ERROR_INVALID_DATA, error))
    return FALSE;

  take_decoded_payload (out_payload, &decoded);
  return TRUE;
}
