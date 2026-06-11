#include <glib.h>
#include <gio/gio.h>

#include "wyrebox-message-delivered-payload.h"

static const char *valid_object_key =
    "sha256:"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

#define V2_OBJECT_KEY_LEN_OFFSET 28
#define V2_OBJECT_KEY_OFFSET 30
#define V1_OBJECT_KEY_LEN_OFFSET 16
#define V1_OBJECT_KEY_OFFSET 18

static const guint8 golden_v1_payload[] = {
  'W', 'Y', 'R', 'E', 'M', 'D', 'P', '1',
  0x39, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x47, 0x00,
  's', 'h', 'a', '2', '5', '6', ':',
  '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
};

static void
assert_decode_fails_invalid_data (GBytes *bytes)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };

  g_assert_false (wyrebox_message_delivered_payload_decode (bytes,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
init_full_metadata (WyreboxEmlMetadata *metadata)
{
  metadata->message_id = "<message@example.test>";
  metadata->subject = "Quarterly report";
  metadata->from = "Alice <alice@example.test>";
  metadata->to = "Bob <bob@example.test>";
  metadata->cc = NULL;
  metadata->bcc = NULL;
  metadata->date = "Tue, 14 Nov 2023 22:13:20 +0000";
  metadata->size_bytes = 12345;
  metadata->duplicate_message_id_count = 2;
}

static void
test_round_trip_full_metadata (void)
{
  WyreboxEmlMetadata metadata = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) reencoded = NULL;
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };

  init_full_metadata (&metadata);

  encoded = wyrebox_message_delivered_payload_encode_full (valid_object_key,
      12345, &metadata, 1700000000123456, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_message_delivered_payload_decode (encoded,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.object_key, ==, valid_object_key);
  g_assert_cmpuint (decoded.size_bytes, ==, 12345);
  g_assert_cmpuint (decoded.internal_date_unix_us, ==, 1700000000123456);
  g_assert_cmpstr (decoded.message_id, ==, metadata.message_id);
  g_assert_cmpstr (decoded.subject, ==, metadata.subject);
  g_assert_cmpstr (decoded.from, ==, metadata.from);
  g_assert_cmpstr (decoded.to, ==, metadata.to);
  g_assert_null (decoded.cc);
  g_assert_null (decoded.bcc);
  g_assert_cmpstr (decoded.date, ==, metadata.date);
  g_assert_cmpuint (decoded.duplicate_message_id_count, ==, 2);

  reencoded = wyrebox_message_delivered_payload_encode_full
      (decoded.object_key, decoded.size_bytes, &metadata,
      decoded.internal_date_unix_us, &error);
  g_assert_no_error (error);
  g_assert_true (g_bytes_equal (encoded, reencoded));
}

static void
test_encoded_format_matches_golden_v2 (void)
{
  const guint8 expected[] = {
    'W', 'Y', 'R', 'E', 'M', 'D', 'P', '2',
    0x39, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x22, 0x20, 0x18, 0x24, 0x0a, 0x06, 0x00,
    0x02, 0x00, 0x00, 0x00,
    0x47, 0x00,
    's', 'h', 'a', '2', '5', '6', ':',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    0x16, 0x00, 0x00, 0x00,
    '<', 'm', 'e', 's', 's', 'a', 'g', 'e',
    '@', 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    '.', 't', 'e', 's', 't', '>',
    0x10, 0x00, 0x00, 0x00,
    'Q', 'u', 'a', 'r', 't', 'e', 'r', 'l',
    'y', ' ', 'r', 'e', 'p', 'o', 'r', 't',
    0x1a, 0x00, 0x00, 0x00,
    'A', 'l', 'i', 'c', 'e', ' ', '<', 'a',
    'l', 'i', 'c', 'e', '@', 'e', 'x', 'a',
    'm', 'p', 'l', 'e', '.', 't', 'e', 's',
    't', '>',
    0x16, 0x00, 0x00, 0x00,
    'B', 'o', 'b', ' ', '<', 'b', 'o', 'b',
    '@', 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    '.', 't', 'e', 's', 't', '>',
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff,
    0x1f, 0x00, 0x00, 0x00,
    'T', 'u', 'e', ',', ' ', '1', '4', ' ',
    'N', 'o', 'v', ' ', '2', '0', '2', '3',
    ' ', '2', '2', ':', '1', '3', ':', '2',
    '0', ' ', '+', '0', '0', '0', '0',
  };
  WyreboxEmlMetadata metadata = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  init_full_metadata (&metadata);

  encoded = wyrebox_message_delivered_payload_encode_full (valid_object_key,
      12345, &metadata, 1700000000123456, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  g_assert_cmpuint (encoded_size, ==, sizeof (expected));
  g_assert_cmpmem (encoded_data, encoded_size, expected, sizeof (expected));
}

static void
test_wrapper_encodes_empty_metadata (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      12345, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_message_delivered_payload_decode (encoded,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.object_key, ==, valid_object_key);
  g_assert_cmpuint (decoded.size_bytes, ==, 12345);
  g_assert_cmpuint (decoded.internal_date_unix_us, ==, 0);
  g_assert_null (decoded.message_id);
  g_assert_null (decoded.subject);
  g_assert_null (decoded.from);
  g_assert_null (decoded.to);
  g_assert_null (decoded.cc);
  g_assert_null (decoded.bcc);
  g_assert_null (decoded.date);
  g_assert_cmpuint (decoded.duplicate_message_id_count, ==, 0);
}

static void
test_decodes_golden_v1_payload (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
      g_bytes_new_static (golden_v1_payload, sizeof (golden_v1_payload));
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };

  g_assert_true (wyrebox_message_delivered_payload_decode (bytes,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.object_key, ==, valid_object_key);
  g_assert_cmpuint (decoded.size_bytes, ==, 12345);
  g_assert_cmpuint (decoded.internal_date_unix_us, ==, 0);
  g_assert_null (decoded.message_id);
  g_assert_null (decoded.subject);
  g_assert_null (decoded.from);
  g_assert_null (decoded.to);
  g_assert_null (decoded.cc);
  g_assert_null (decoded.bcc);
  g_assert_null (decoded.date);
  g_assert_cmpuint (decoded.duplicate_message_id_count, ==, 0);
}

static void
test_rejects_invalid_key_on_encode (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  encoded = wyrebox_message_delivered_payload_encode ("sha256:"
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg",
      12345, &error);

  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_rejects_v1_trailing_bytes (void)
{
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  gsize payload_size = sizeof (golden_v1_payload);

  copy = g_malloc (payload_size + 1);
  memcpy (copy, golden_v1_payload, payload_size);
  copy[payload_size] = 0;
  malformed = g_bytes_new_take (g_steal_pointer (&copy), payload_size + 1);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_v1_malformed_payload_length (void)
{
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  gsize payload_size = sizeof (golden_v1_payload);

  copy = g_memdup2 (golden_v1_payload, payload_size);
  copy[V1_OBJECT_KEY_LEN_OFFSET] = 0x48;
  malformed = g_bytes_new_take (g_steal_pointer (&copy), payload_size);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_v1_object_key_with_nul_padding (void)
{
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  gsize payload_size = sizeof (golden_v1_payload);
  guint16 padded_object_key_len = strlen (valid_object_key) + 1;

  copy = g_malloc (payload_size + 1);
  memcpy (copy, golden_v1_payload, payload_size);
  copy[V1_OBJECT_KEY_LEN_OFFSET] = (guint8) (padded_object_key_len & 0xff);
  copy[V1_OBJECT_KEY_LEN_OFFSET + 1] =
      (guint8) ((padded_object_key_len >> 8) & 0xff);
  copy[V1_OBJECT_KEY_OFFSET + strlen (valid_object_key)] = '\0';
  malformed = g_bytes_new_take (g_steal_pointer (&copy), payload_size + 1);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_invalid_prefix_on_decode (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      12345, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  copy = g_memdup2 (encoded_data, encoded_size);
  copy[V2_OBJECT_KEY_OFFSET + 5] = '7';
  malformed = g_bytes_new_take (g_steal_pointer (&copy), encoded_size);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_wrong_object_key_length_on_decode (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      12345, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  copy = g_memdup2 (encoded_data, encoded_size);
  copy[V2_OBJECT_KEY_LEN_OFFSET] = 0x46;
  malformed = g_bytes_new_take (g_steal_pointer (&copy), encoded_size);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_v2_object_key_with_nul_padding (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;
  gsize object_key_len = strlen (valid_object_key);
  gsize metadata_offset = V2_OBJECT_KEY_OFFSET + object_key_len;
  guint16 padded_object_key_len = object_key_len + 1;

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      12345, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  copy = g_malloc (encoded_size + 1);
  memcpy (copy, encoded_data, metadata_offset);
  copy[V2_OBJECT_KEY_LEN_OFFSET] = (guint8) (padded_object_key_len & 0xff);
  copy[V2_OBJECT_KEY_LEN_OFFSET + 1] =
      (guint8) ((padded_object_key_len >> 8) & 0xff);
  copy[metadata_offset] = '\0';
  memcpy (copy + metadata_offset + 1, encoded_data + metadata_offset,
      encoded_size - metadata_offset);
  malformed = g_bytes_new_take (g_steal_pointer (&copy), encoded_size + 1);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_uppercase_hex_on_decode (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      12345, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  copy = g_memdup2 (encoded_data, encoded_size);
  copy[V2_OBJECT_KEY_OFFSET + strlen ("sha256:")] = 'A';
  malformed = g_bytes_new_take (g_steal_pointer (&copy), encoded_size);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_trailing_bytes (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      12345, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  copy = g_malloc (encoded_size + 1);
  memcpy (copy, encoded_data, encoded_size);
  copy[encoded_size] = 0;
  malformed = g_bytes_new_take (g_steal_pointer (&copy), encoded_size + 1);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_truncated_payload (void)
{
  const guint8 payload[] = {
    'W', 'Y', 'R', 'E', 'M', 'D', 'P', '1',
    0x39,
  };
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, sizeof (payload));

  assert_decode_fails_invalid_data (bytes);
}

static void
test_rejects_malformed_payload_length (void)
{
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  g_autoptr (GError) error = NULL;
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      12345, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  copy = g_memdup2 (encoded_data, encoded_size);
  copy[V2_OBJECT_KEY_LEN_OFFSET] = 0x48;
  malformed = g_bytes_new_take (g_steal_pointer (&copy), encoded_size);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_truncated_object_key_body (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      12345, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  malformed = g_bytes_new (encoded_data, V2_OBJECT_KEY_OFFSET + 8);

  g_assert_cmpuint (encoded_size, >, V2_OBJECT_KEY_OFFSET + 8);
  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_metadata_string_with_embedded_nul (void)
{
  WyreboxEmlMetadata metadata = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;
  gsize message_id_offset = V2_OBJECT_KEY_OFFSET + strlen (valid_object_key);

  metadata.message_id = "abc";

  encoded = wyrebox_message_delivered_payload_encode_full (valid_object_key,
      12345, &metadata, 0, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  copy = g_memdup2 (encoded_data, encoded_size);
  copy[message_id_offset + sizeof (guint32) + 1] = '\0';
  malformed = g_bytes_new_take (g_steal_pointer (&copy), encoded_size);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_truncated_metadata_string_body (void)
{
  WyreboxEmlMetadata metadata = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  init_full_metadata (&metadata);

  encoded = wyrebox_message_delivered_payload_encode_full (valid_object_key,
      12345, &metadata, 1700000000123456, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  malformed = g_bytes_new (encoded_data, encoded_size - 1);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_empty_payload (void)
{
  g_autoptr (GBytes) bytes = g_bytes_new_static ("", 0);

  assert_decode_fails_invalid_data (bytes);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/message-delivered-payload/round-trip-full-metadata",
      test_round_trip_full_metadata);
  g_test_add_func
      ("/message-delivered-payload/encoded-format-matches-golden-v2",
      test_encoded_format_matches_golden_v2);
  g_test_add_func ("/message-delivered-payload/wrapper-encodes-empty-metadata",
      test_wrapper_encodes_empty_metadata);
  g_test_add_func ("/message-delivered-payload/decodes-golden-v1-payload",
      test_decodes_golden_v1_payload);
  g_test_add_func ("/message-delivered-payload/rejects-invalid-key-on-encode",
      test_rejects_invalid_key_on_encode);
  g_test_add_func ("/message-delivered-payload/rejects-v1-trailing-bytes",
      test_rejects_v1_trailing_bytes);
  g_test_add_func
      ("/message-delivered-payload/rejects-v1-malformed-payload-length",
      test_rejects_v1_malformed_payload_length);
  g_test_add_func ("/message-delivered-payload/"
      "rejects-v1-object-key-with-nul-padding",
      test_rejects_v1_object_key_with_nul_padding);
  g_test_add_func
      ("/message-delivered-payload/rejects-invalid-prefix-on-decode",
      test_rejects_invalid_prefix_on_decode);
  g_test_add_func ("/message-delivered-payload/"
      "rejects-wrong-object-key-length-on-decode",
      test_rejects_wrong_object_key_length_on_decode);
  g_test_add_func ("/message-delivered-payload/"
      "rejects-v2-object-key-with-nul-padding",
      test_rejects_v2_object_key_with_nul_padding);
  g_test_add_func ("/message-delivered-payload/rejects-uppercase-hex-on-decode",
      test_rejects_uppercase_hex_on_decode);
  g_test_add_func ("/message-delivered-payload/rejects-trailing-bytes",
      test_rejects_trailing_bytes);
  g_test_add_func ("/message-delivered-payload/rejects-truncated-payload",
      test_rejects_truncated_payload);
  g_test_add_func ("/message-delivered-payload/rejects-malformed-length",
      test_rejects_malformed_payload_length);
  g_test_add_func
      ("/message-delivered-payload/rejects-truncated-object-key-body",
      test_rejects_truncated_object_key_body);
  g_test_add_func
      ("/message-delivered-payload/rejects-truncated-metadata-string-body",
      test_rejects_truncated_metadata_string_body);
  g_test_add_func ("/message-delivered-payload/"
      "rejects-metadata-string-with-embedded-nul",
      test_rejects_metadata_string_with_embedded_nul);
  g_test_add_func ("/message-delivered-payload/rejects-empty-payload",
      test_rejects_empty_payload);

  return g_test_run ();
}
