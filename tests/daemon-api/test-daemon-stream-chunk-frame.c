#include "wyrebox-daemon-stream-chunk-frame.h"

#include <gio/gio.h>
#include <glib.h>

static GBytes *
make_bytes (const guint8 *data, gsize size)
{
  return g_bytes_new (data, size);
}

static void
test_stream_chunk_init_copies_payload_and_identity (void)
{
  guint8 payload[] = { 0x01, 0x02, 0x03 };
  g_autoptr (GBytes) bytes = make_bytes (payload, sizeof (payload));
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonStreamChunkFrame) frame = { 0 };
  const guint8 *input = NULL;
  const guint8 *stored = NULL;
  gsize input_size = 0;
  gsize stored_size = 0;

  input = g_bytes_get_data (bytes, &input_size);
  g_assert_true (wyrebox_daemon_stream_chunk_frame_init (&frame,
          "request-stream", "message-1", NULL, "correlation-1", 7,
          bytes, FALSE, &error));
  g_assert_no_error (error);

  payload[0] = 0xff;
  stored = g_bytes_get_data (frame.bytes, &stored_size);

  g_assert_cmpstr (frame.request_id, ==, "request-stream");
  g_assert_cmpstr (frame.message_id, ==, "message-1");
  g_assert_null (frame.query_id);
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpuint (frame.chunk_index, ==, 7);
  g_assert_false (frame.end_of_stream);
  g_assert_cmpuint (input_size, ==, 3);
  g_assert_cmpuint (stored_size, ==, 3);
  g_assert_true (stored != input);
  g_assert_cmpuint (stored[0], ==, 0x01);
  g_assert_cmpuint (stored[1], ==, 0x02);
  g_assert_cmpuint (stored[2], ==, 0x03);
}

static void
test_stream_chunk_accepts_empty_final_chunk (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonStreamChunkFrame) frame = { 0 };
  gsize stored_size = 1;

  g_assert_true (wyrebox_daemon_stream_chunk_frame_init (&frame,
          "request-stream", NULL, "query-1", NULL, 8, NULL, TRUE, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (frame.request_id, ==, "request-stream");
  g_assert_null (frame.message_id);
  g_assert_cmpstr (frame.query_id, ==, "query-1");
  g_assert_null (frame.correlation_id);
  g_assert_cmpuint (frame.chunk_index, ==, 8);
  g_assert_true (frame.end_of_stream);
  (void) g_bytes_get_data (frame.bytes, &stored_size);
  g_assert_cmpuint (stored_size, ==, 0);
}

static void
test_stream_chunk_rejects_missing_request_id (void)
{
  guint8 payload[] = { 0x01 };
  g_autoptr (GBytes) bytes = make_bytes (payload, sizeof (payload));
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonStreamChunkFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_stream_chunk_frame_init (&frame,
          "", "message-1", NULL, NULL, 0, bytes, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (frame.request_id);
  g_assert_null (frame.bytes);
}

static void
test_stream_chunk_rejects_missing_discriminator (void)
{
  guint8 payload[] = { 0x01 };
  g_autoptr (GBytes) bytes = make_bytes (payload, sizeof (payload));
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonStreamChunkFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_stream_chunk_frame_init (&frame,
          "request-stream", NULL, "", NULL, 0, bytes, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (frame.request_id);
  g_assert_null (frame.bytes);
}

static void
test_stream_chunk_rejects_ambiguous_discriminator (void)
{
  guint8 payload[] = { 0x01 };
  g_autoptr (GBytes) bytes = make_bytes (payload, sizeof (payload));
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonStreamChunkFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_stream_chunk_frame_init (&frame,
          "request-stream", "message-1", "query-1", NULL, 0, bytes, FALSE,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (frame.request_id);
  g_assert_null (frame.bytes);
}

static void
test_stream_chunk_rejects_control_characters (void)
{
  guint8 payload[] = { 0x01 };
  g_autoptr (GBytes) bytes = make_bytes (payload, sizeof (payload));
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonStreamChunkFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_stream_chunk_frame_init (&frame,
          "request-stream", "message\n1", NULL, NULL, 0, bytes, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (frame.request_id);
  g_assert_null (frame.bytes);
}

static void
test_stream_chunk_rejects_empty_non_final_bytes (void)
{
  g_autoptr (GBytes) bytes = g_bytes_new_static ("", 0);
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonStreamChunkFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_stream_chunk_frame_init (&frame,
          "request-stream", "message-1", NULL, NULL, 0, bytes, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (frame.request_id);
  g_assert_null (frame.bytes);
}

static void
test_stream_chunk_failure_leaves_existing_contents (void)
{
  guint8 payload[] = { 0x01 };
  g_autoptr (GBytes) bytes = make_bytes (payload, sizeof (payload));
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonStreamChunkFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_stream_chunk_frame_init (&frame,
          "request-stream", "message-1", NULL, "correlation-1", 3, bytes,
          FALSE, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_stream_chunk_frame_init (&frame,
          "", "message-2", NULL, NULL, 0, bytes, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_assert_cmpstr (frame.request_id, ==, "request-stream");
  g_assert_cmpstr (frame.message_id, ==, "message-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpuint (frame.chunk_index, ==, 3);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/stream-chunk/init-copies-payload-and-identity",
      test_stream_chunk_init_copies_payload_and_identity);
  g_test_add_func ("/daemon-api/stream-chunk/accepts-empty-final-chunk",
      test_stream_chunk_accepts_empty_final_chunk);
  g_test_add_func ("/daemon-api/stream-chunk/rejects-missing-request-id",
      test_stream_chunk_rejects_missing_request_id);
  g_test_add_func ("/daemon-api/stream-chunk/rejects-missing-discriminator",
      test_stream_chunk_rejects_missing_discriminator);
  g_test_add_func ("/daemon-api/stream-chunk/rejects-ambiguous-discriminator",
      test_stream_chunk_rejects_ambiguous_discriminator);
  g_test_add_func ("/daemon-api/stream-chunk/rejects-control-characters",
      test_stream_chunk_rejects_control_characters);
  g_test_add_func ("/daemon-api/stream-chunk/rejects-empty-non-final-bytes",
      test_stream_chunk_rejects_empty_non_final_bytes);
  g_test_add_func ("/daemon-api/stream-chunk/failure-leaves-existing",
      test_stream_chunk_failure_leaves_existing_contents);

  return g_test_run ();
}
