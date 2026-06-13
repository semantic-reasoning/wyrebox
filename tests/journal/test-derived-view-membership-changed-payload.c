#include "wyrebox-derived-view-membership-changed-payload.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#define HEADER_SIZE 33

static WyreboxDerivedViewMembershipChangedPayload
valid_payload (gboolean is_visible)
{
  return (WyreboxDerivedViewMembershipChangedPayload) {
  .account_id = (char *) "account-1",.view_id =
        (char *) "view-alpha",.message_id =
        (char *) "message-1",.membership_id =
        (char *) "derived-view:sha256:"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",.rule_version_hash
        =
        (char *) "sha256:"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",.uid
        = 7,.uidvalidity = 1,.is_visible =
        is_visible,.materialized_at_unix_us = 1700000000123456,};
}

static void
write_u32_le (GByteArray *array, guint32 value)
{
  guint8 bytes[4] = {
    (guint8) ((value >> 0) & 0xff),
    (guint8) ((value >> 8) & 0xff),
    (guint8) ((value >> 16) & 0xff),
    (guint8) ((value >> 24) & 0xff),
  };

  g_byte_array_append (array, bytes, sizeof (bytes));
}

static void
write_u64_at (guint8 *data, gsize offset, guint64 value)
{
  data[offset + 0] = (guint8) ((value >> 0) & 0xff);
  data[offset + 1] = (guint8) ((value >> 8) & 0xff);
  data[offset + 2] = (guint8) ((value >> 16) & 0xff);
  data[offset + 3] = (guint8) ((value >> 24) & 0xff);
  data[offset + 4] = (guint8) ((value >> 32) & 0xff);
  data[offset + 5] = (guint8) ((value >> 40) & 0xff);
  data[offset + 6] = (guint8) ((value >> 48) & 0xff);
  data[offset + 7] = (guint8) ((value >> 56) & 0xff);
}

static void
append_required_string (GByteArray *array, const char *value)
{
  gsize len = strlen (value);

  g_assert_cmpuint (len, <=, G_MAXUINT32);
  write_u32_le (array, (guint32) len);
  g_byte_array_append (array, (const guint8 *) value, len);
}

static GBytes *
encode_valid_payload (gboolean is_visible)
{
  WyreboxDerivedViewMembershipChangedPayload payload =
      valid_payload (is_visible);
  g_autoptr (GError) error = NULL;
  GBytes *encoded = NULL;

  encoded = wyrebox_derived_view_membership_changed_payload_encode (&payload,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);
  return encoded;
}

static void
assert_decode_fails_invalid_data (GBytes *bytes)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDerivedViewMembershipChangedPayload) decoded = { 0 };

  g_assert_false (wyrebox_derived_view_membership_changed_payload_decode
      (bytes, &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
assert_round_trip (gboolean is_visible)
{
  WyreboxDerivedViewMembershipChangedPayload payload =
      valid_payload (is_visible);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) reencoded = NULL;
  g_auto (WyreboxDerivedViewMembershipChangedPayload) decoded = { 0 };

  encoded = wyrebox_derived_view_membership_changed_payload_encode (&payload,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);
  g_assert_true (wyrebox_derived_view_membership_changed_payload_decode
      (encoded, &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.account_id, ==, payload.account_id);
  g_assert_cmpstr (decoded.view_id, ==, payload.view_id);
  g_assert_cmpstr (decoded.message_id, ==, payload.message_id);
  g_assert_cmpstr (decoded.membership_id, ==, payload.membership_id);
  g_assert_cmpstr (decoded.rule_version_hash, ==, payload.rule_version_hash);
  g_assert_cmpuint (decoded.uid, ==, payload.uid);
  g_assert_cmpuint (decoded.uidvalidity, ==, payload.uidvalidity);
  g_assert_cmpint (decoded.is_visible, ==, payload.is_visible);
  g_assert_cmpuint (decoded.materialized_at_unix_us, ==,
      payload.materialized_at_unix_us);

  reencoded =
      wyrebox_derived_view_membership_changed_payload_encode (&decoded, &error);
  g_assert_no_error (error);
  g_assert_true (g_bytes_equal (encoded, reencoded));
}

static void
test_round_trip_visible (void)
{
  assert_round_trip (TRUE);
}

static void
test_round_trip_hidden (void)
{
  assert_round_trip (FALSE);
}

static void
test_encoded_header_matches_golden (void)
{
  g_autoptr (GBytes) encoded = encode_valid_payload (TRUE);
  const guint8 *data = NULL;
  gsize size = 0;
  static const guint8 expected_header[] = {
    'W', 'Y', 'R', 'E', 'D', 'V', 'C', '1',
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01,
    0x40, 0x22, 0x20, 0x18, 0x24, 0x0a, 0x06, 0x00,
    0x09, 0x00, 0x00, 0x00,
    'a', 'c', 'c', 'o', 'u', 'n', 't', '-', '1',
  };

  data = g_bytes_get_data (encoded, &size);
  g_assert_cmpuint (size, >, sizeof (expected_header));
  g_assert_cmpmem (data, sizeof (expected_header), expected_header,
      sizeof (expected_header));
}

static void
test_encode_rejects_invalid_arguments (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  WyreboxDerivedViewMembershipChangedPayload payload = valid_payload (TRUE);

  payload.account_id = NULL;
  encoded = wyrebox_derived_view_membership_changed_payload_encode (&payload,
      &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  payload = valid_payload (TRUE);
  payload.view_id = (char *) "";
  encoded = wyrebox_derived_view_membership_changed_payload_encode (&payload,
      &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  payload = valid_payload (TRUE);
  payload.uid = 0;
  encoded = wyrebox_derived_view_membership_changed_payload_encode (&payload,
      &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  payload = valid_payload (TRUE);
  payload.uidvalidity = 0;
  encoded = wyrebox_derived_view_membership_changed_payload_encode (&payload,
      &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  payload = valid_payload (TRUE);
  payload.materialized_at_unix_us = 0;
  encoded = wyrebox_derived_view_membership_changed_payload_encode (&payload,
      &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_decode_rejects_bad_magic_and_version (void)
{
  g_autoptr (GBytes) encoded = encode_valid_payload (TRUE);
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (encoded, &size);
  g_autofree guint8 *copy = g_memdup2 (data, size);
  g_autoptr (GBytes) bad_magic = NULL;
  g_autoptr (GBytes) bad_version = NULL;

  copy[0] = 'X';
  bad_magic = g_bytes_new (copy, size);
  assert_decode_fails_invalid_data (bad_magic);

  memcpy (copy, data, size);
  copy[7] = '2';
  bad_version = g_bytes_new (copy, size);
  assert_decode_fails_invalid_data (bad_version);
}

static void
test_decode_rejects_truncated_header (void)
{
  static const guint8 bytes[] = {
    'W', 'Y', 'R', 'E', 'D', 'V', 'C', '1',
    0x01,
  };
  g_autoptr (GBytes) truncated = g_bytes_new_static (bytes, sizeof (bytes));

  assert_decode_fails_invalid_data (truncated);
}

static void
test_decode_rejects_truncated_string (void)
{
  g_autoptr (GBytes) encoded = encode_valid_payload (TRUE);
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (encoded, &size);
  g_autoptr (GBytes) truncated = g_bytes_new (data, size - 1);

  assert_decode_fails_invalid_data (truncated);
}

static void
test_decode_rejects_embedded_nul_string (void)
{
  g_autoptr (GBytes) encoded = encode_valid_payload (TRUE);
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (encoded, &size);
  g_autofree guint8 *copy = g_memdup2 (data, size);
  g_autoptr (GBytes) bad = NULL;

  copy[HEADER_SIZE + 4] = '\0';
  bad = g_bytes_new (copy, size);
  assert_decode_fails_invalid_data (bad);
}

static void
test_decode_rejects_trailing_bytes (void)
{
  g_autoptr (GBytes) encoded = encode_valid_payload (TRUE);
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (encoded, &size);
  g_autoptr (GByteArray) array = g_byte_array_new ();
  g_autoptr (GBytes) bad = NULL;
  const guint8 extra = 0xff;

  g_byte_array_append (array, data, size);
  g_byte_array_append (array, &extra, sizeof (extra));
  bad = g_byte_array_free_to_bytes (g_steal_pointer (&array));
  assert_decode_fails_invalid_data (bad);
}

static void
test_decode_rejects_zero_numeric_fields (void)
{
  g_autoptr (GBytes) encoded = encode_valid_payload (TRUE);
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (encoded, &size);
  g_autofree guint8 *copy = g_memdup2 (data, size);
  g_autoptr (GBytes) bad_uid = NULL;
  g_autoptr (GBytes) bad_uidvalidity = NULL;
  g_autoptr (GBytes) bad_materialized_at = NULL;

  memset (copy + 8, 0, sizeof (guint64));
  bad_uid = g_bytes_new (copy, size);
  assert_decode_fails_invalid_data (bad_uid);

  memcpy (copy, data, size);
  memset (copy + 16, 0, sizeof (guint64));
  bad_uidvalidity = g_bytes_new (copy, size);
  assert_decode_fails_invalid_data (bad_uidvalidity);

  memcpy (copy, data, size);
  memset (copy + 25, 0, sizeof (guint64));
  bad_materialized_at = g_bytes_new (copy, size);
  assert_decode_fails_invalid_data (bad_materialized_at);
}

static void
test_decode_rejects_empty_required_string (void)
{
  g_autoptr (GByteArray) array = g_byte_array_new ();
  g_autoptr (GBytes) bad = NULL;

  g_byte_array_set_size (array, HEADER_SIZE);
  memcpy (array->data, "WYREDVC1", 8);
  write_u64_at (array->data, 8, 7);
  write_u64_at (array->data, 16, 1);
  array->data[24] = 1;
  write_u64_at (array->data, 25, 1700000000123456);
  write_u32_le (array, 0);
  append_required_string (array, "view-alpha");
  append_required_string (array, "message-1");
  append_required_string (array, "membership-1");
  append_required_string (array, "sha256:rule");

  bad = g_byte_array_free_to_bytes (g_steal_pointer (&array));
  assert_decode_fails_invalid_data (bad);
}

static void
test_decode_rejects_malformed_visibility (void)
{
  g_autoptr (GBytes) encoded = encode_valid_payload (TRUE);
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (encoded, &size);
  g_autofree guint8 *copy = g_memdup2 (data, size);
  g_autoptr (GBytes) bad = NULL;

  copy[24] = 2;
  bad = g_bytes_new (copy, size);
  assert_decode_fails_invalid_data (bad);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/journal/derived-view-membership-changed/roundtrip-visible",
      test_round_trip_visible);
  g_test_add_func ("/journal/derived-view-membership-changed/roundtrip-hidden",
      test_round_trip_hidden);
  g_test_add_func ("/journal/derived-view-membership-changed/golden-header",
      test_encoded_header_matches_golden);
  g_test_add_func ("/journal/derived-view-membership-changed/encode-invalid",
      test_encode_rejects_invalid_arguments);
  g_test_add_func ("/journal/derived-view-membership-changed/decode-magic",
      test_decode_rejects_bad_magic_and_version);
  g_test_add_func ("/journal/derived-view-membership-changed/decode-header",
      test_decode_rejects_truncated_header);
  g_test_add_func ("/journal/derived-view-membership-changed/decode-string",
      test_decode_rejects_truncated_string);
  g_test_add_func ("/journal/derived-view-membership-changed/decode-nul",
      test_decode_rejects_embedded_nul_string);
  g_test_add_func ("/journal/derived-view-membership-changed/decode-trailing",
      test_decode_rejects_trailing_bytes);
  g_test_add_func ("/journal/derived-view-membership-changed/decode-zero",
      test_decode_rejects_zero_numeric_fields);
  g_test_add_func ("/journal/derived-view-membership-changed/decode-empty",
      test_decode_rejects_empty_required_string);
  g_test_add_func ("/journal/derived-view-membership-changed/decode-visible",
      test_decode_rejects_malformed_visibility);

  return g_test_run ();
}
