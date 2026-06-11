#include <glib.h>
#include <gio/gio.h>

#include "wyrebox-message-delivered-payload.h"

static const char *valid_object_key =
    "sha256:"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

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
test_round_trip (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) reencoded = NULL;
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

  reencoded = wyrebox_message_delivered_payload_encode (decoded.object_key,
      decoded.size_bytes, &error);
  g_assert_no_error (error);
  g_assert_true (g_bytes_equal (encoded, reencoded));
}

static void
test_encoded_format_matches_golden_v1 (void)
{
  const guint8 expected[] = {
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
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      12345, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  g_assert_cmpuint (encoded_size, ==, sizeof (expected));
  g_assert_cmpmem (encoded_data, encoded_size, expected, sizeof (expected));
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
test_rejects_invalid_prefix_on_decode (void)
{
  const guint8 payload[] = {
    'W', 'Y', 'R', 'E', 'M', 'D', 'P', '1',
    0x39, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x00,
    's', 'h', 'a', '2', '5', '7', ':',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, sizeof (payload));

  assert_decode_fails_invalid_data (bytes);
}

static void
test_rejects_wrong_object_key_length_on_decode (void)
{
  const guint8 payload[] = {
    'W', 'Y', 'R', 'E', 'M', 'D', 'P', '1',
    0x39, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x46, 0x00,
    's', 'h', 'a', '2', '5', '6', ':',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e',
  };
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, sizeof (payload));

  assert_decode_fails_invalid_data (bytes);
}

static void
test_rejects_uppercase_hex_on_decode (void)
{
  const guint8 payload[] = {
    'W', 'Y', 'R', 'E', 'M', 'D', 'P', '1',
    0x39, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x00,
    's', 'h', 'a', '2', '5', '6', ':',
    'A', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, sizeof (payload));

  assert_decode_fails_invalid_data (bytes);
}

static void
test_rejects_trailing_bytes (void)
{
  const guint8 payload[] = {
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
    0x00,
  };
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, sizeof (payload));

  assert_decode_fails_invalid_data (bytes);
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
  copy[16] = 0x48;
  malformed = g_bytes_new_take (g_steal_pointer (&copy), encoded_size);

  assert_decode_fails_invalid_data (malformed);
}

static void
test_rejects_truncated_object_key_body (void)
{
  const guint8 payload[] = {
    'W', 'Y', 'R', 'E', 'M', 'D', 'P', '1',
    0x39, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x00,
    's', 'h', 'a', '2', '5', '6', ':',
    '0', '1', '2', '3', '4', '5', '6', '7',
  };
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, sizeof (payload));

  assert_decode_fails_invalid_data (bytes);
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

  g_test_add_func ("/message-delivered-payload/round-trip", test_round_trip);
  g_test_add_func
      ("/message-delivered-payload/encoded-format-matches-golden-v1",
      test_encoded_format_matches_golden_v1);
  g_test_add_func ("/message-delivered-payload/rejects-invalid-key-on-encode",
      test_rejects_invalid_key_on_encode);
  g_test_add_func
      ("/message-delivered-payload/rejects-invalid-prefix-on-decode",
      test_rejects_invalid_prefix_on_decode);
  g_test_add_func ("/message-delivered-payload/"
      "rejects-wrong-object-key-length-on-decode",
      test_rejects_wrong_object_key_length_on_decode);
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
  g_test_add_func ("/message-delivered-payload/rejects-empty-payload",
      test_rejects_empty_payload);

  return g_test_run ();
}
