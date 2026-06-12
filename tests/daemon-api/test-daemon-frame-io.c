#include "wyrebox-daemon-frame-io.h"

#include <gio/gio.h>
#include <glib.h>

#include <string.h>

static void
assert_payload_bytes_equal (GBytes *actual,
    const guint8 *expected, gsize expected_size)
{
  gsize actual_size = 0;
  const guint8 *actual_data = NULL;

  g_assert_nonnull (actual);
  actual_data = g_bytes_get_data (actual, &actual_size);
  g_assert_cmpuint (actual_size, ==, expected_size);
  g_assert_cmpint (memcmp (actual_data, expected, expected_size), ==, 0);
}

static void
test_frame_io_round_trip_preserves_embedded_nuls (void)
{
  const guint8 payload[] = {
    0x00, 'h', 0x00, 'i', 'A', 0x00, 'B', 0xFF, 0x00, 'Z',
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GOutputStream) output = NULL;
  const guint8 *frame_data = NULL;
  gsize frame_size = 0;
  g_autoptr (GInputStream) input = NULL;
  g_autoptr (GBytes) read_payload = NULL;

  output = G_OUTPUT_STREAM (g_memory_output_stream_new_resizable ());
  g_assert_true (wyrebox_daemon_frame_io_write_payload (G_OUTPUT_STREAM
          (output), payload, sizeof (payload), &error));
  g_assert_no_error (error);

  frame_size =
      g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (output));
  g_assert_cmpuint (frame_size, ==, sizeof (payload) + 4);
  frame_data =
      g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (output));
  g_assert_nonnull (frame_data);

  input = g_memory_input_stream_new_from_data (frame_data, frame_size, NULL);
  g_assert_nonnull (input);

  read_payload = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_no_error (error);
  assert_payload_bytes_equal (read_payload, payload, sizeof (payload));
}

static void
test_frame_io_supports_multiple_sequential_frames (void)
{
  const guint8 first_payload[] = "frame one";
  const guint8 second_payload[] = "frame two";
  g_autoptr (GError) error = NULL;
  g_autoptr (GOutputStream) output = NULL;
  const guint8 *frame_data = NULL;
  gsize frame_size = 0;
  g_autoptr (GInputStream) input = NULL;
  g_autoptr (GBytes) first_read = NULL;
  g_autoptr (GBytes) second_read = NULL;

  output = G_OUTPUT_STREAM (g_memory_output_stream_new_resizable ());
  g_assert_true (wyrebox_daemon_frame_io_write_payload (G_OUTPUT_STREAM
          (output), first_payload, sizeof (first_payload), &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_frame_io_write_payload (G_OUTPUT_STREAM
          (output), second_payload, sizeof (second_payload), &error));
  g_assert_no_error (error);

  frame_size =
      g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (output));
  g_assert_cmpuint (frame_size, ==,
      sizeof (first_payload) + sizeof (second_payload)
      + 8);
  frame_data =
      g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (output));
  g_assert_nonnull (frame_data);

  input = g_memory_input_stream_new_from_data (frame_data, frame_size, NULL);
  g_assert_nonnull (input);

  first_read = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_no_error (error);
  assert_payload_bytes_equal (first_read, first_payload,
      sizeof (first_payload));

  second_read = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_no_error (error);
  assert_payload_bytes_equal (second_read,
      second_payload, sizeof (second_payload));
}

static void
test_frame_io_rejects_truncated_length_prefix (void)
{
  const guint8 truncated_prefix[] = { 0x00, 0x00, 0x00 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  g_autoptr (GInputStream) input = NULL;

  input = g_memory_input_stream_new_from_data (truncated_prefix,
      sizeof (truncated_prefix), NULL);
  g_assert_nonnull (input);

  payload = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_null (payload);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_frame_io_rejects_truncated_payload (void)
{
  const guint8 truncated_payload_prefix[] = {
    0x00, 0x00, 0x00, 0x05, 'a', 'b', 'c',
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  g_autoptr (GInputStream) input = NULL;

  input = g_memory_input_stream_new_from_data (truncated_payload_prefix,
      sizeof (truncated_payload_prefix), NULL);
  g_assert_nonnull (input);

  payload = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_null (payload);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_frame_io_rejects_oversized_prefix (void)
{
  guint8 serialized[4];
  guint8 oversized_prefix[4];
  gsize oversized = WYREBOX_DAEMON_MAX_FRAME_SIZE_BYTES + 1;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  g_autoptr (GInputStream) input = NULL;

  oversized_prefix[0] = (guint8) (oversized >> 24);
  oversized_prefix[1] = (guint8) (oversized >> 16);
  oversized_prefix[2] = (guint8) (oversized >> 8);
  oversized_prefix[3] = (guint8) oversized;

  memcpy (serialized, oversized_prefix, sizeof (oversized_prefix));

  input =
      g_memory_input_stream_new_from_data (serialized,
      sizeof (oversized_prefix), NULL);
  g_assert_nonnull (input);

  payload = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_null (payload);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_frame_io_rejects_zero_length_payload (void)
{
  const guint8 payload[1] = { '\0' };
  g_autoptr (GError) error = NULL;
  g_autoptr (GOutputStream) output = NULL;

  output = G_OUTPUT_STREAM (g_memory_output_stream_new_resizable ());

  g_assert_false (wyrebox_daemon_frame_io_write_payload (G_OUTPUT_STREAM
          (output), payload, 0, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_frame_io_propagates_output_write_failure (void)
{
  const guint8 payload[] = "closed stream failure";
  g_autoptr (GError) error = NULL;
  g_autoptr (GOutputStream) output = NULL;

  output = G_OUTPUT_STREAM (g_memory_output_stream_new_resizable ());
  g_assert_true (g_output_stream_close (G_OUTPUT_STREAM (output), NULL,
          &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_frame_io_write_payload (G_OUTPUT_STREAM
          (output), payload, sizeof (payload), &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/frame-io/round-trip-preserves-embedded-nuls",
      test_frame_io_round_trip_preserves_embedded_nuls);
  g_test_add_func ("/daemon-api/frame-io/multiple-sequential-frames",
      test_frame_io_supports_multiple_sequential_frames);
  g_test_add_func ("/daemon-api/frame-io/rejects-truncated-length-prefix",
      test_frame_io_rejects_truncated_length_prefix);
  g_test_add_func ("/daemon-api/frame-io/rejects-truncated-payload",
      test_frame_io_rejects_truncated_payload);
  g_test_add_func ("/daemon-api/frame-io/rejects-oversized-prefix",
      test_frame_io_rejects_oversized_prefix);
  g_test_add_func ("/daemon-api/frame-io/rejects-zero-length-payload",
      test_frame_io_rejects_zero_length_payload);
  g_test_add_func ("/daemon-api/frame-io/propagates-output-write-failure",
      test_frame_io_propagates_output_write_failure);

  return g_test_run ();
}
