#include <glib.h>
#include <gio/gio.h>

#include "wyrebox-message-delivered-payload.h"

static const char *valid_object_key =
    "sha256:"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

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
test_rejects_invalid_key_on_decode (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      12345, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  copy = g_memdup2 (encoded_data, encoded_size);
  copy[encoded_size - 1] = 'G';
  malformed = g_bytes_new_take (g_steal_pointer (&copy), encoded_size);

  g_assert_false (wyrebox_message_delivered_payload_decode (malformed,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_rejects_truncated_payload (void)
{
  const guint8 payload[] = {
    'W', 'Y', 'R', 'E', 'M', 'D', 'P', '1',
    0x39,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, sizeof (payload));
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };

  g_assert_false (wyrebox_message_delivered_payload_decode (bytes,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_rejects_malformed_payload_length (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      12345, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  copy = g_memdup2 (encoded_data, encoded_size);
  copy[16] = 0x48;
  malformed = g_bytes_new_take (g_steal_pointer (&copy), encoded_size);

  g_assert_false (wyrebox_message_delivered_payload_decode (malformed,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_rejects_empty_payload (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = g_bytes_new_static ("", 0);
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };

  g_assert_false (wyrebox_message_delivered_payload_decode (bytes,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/message-delivered-payload/round-trip", test_round_trip);
  g_test_add_func ("/message-delivered-payload/rejects-invalid-key-on-encode",
      test_rejects_invalid_key_on_encode);
  g_test_add_func ("/message-delivered-payload/rejects-invalid-key-on-decode",
      test_rejects_invalid_key_on_decode);
  g_test_add_func ("/message-delivered-payload/rejects-truncated-payload",
      test_rejects_truncated_payload);
  g_test_add_func ("/message-delivered-payload/rejects-malformed-length",
      test_rejects_malformed_payload_length);
  g_test_add_func ("/message-delivered-payload/rejects-empty-payload",
      test_rejects_empty_payload);

  return g_test_run ();
}
