#include "wyrebox-daemon-frame-io.h"

#include <gio/gio.h>

#include <string.h>

#define WYREBOX_DAEMON_FRAME_PREFIX_BYTES sizeof (guint32)

static inline void
write_u32_be (guint8 *dst, guint32 value)
{
  dst[0] = (guint8) (value >> 24);
  dst[1] = (guint8) (value >> 16);
  dst[2] = (guint8) (value >> 8);
  dst[3] = (guint8) value;
}

static inline guint32
read_u32_be (const guint8 *data)
{
  return ((guint32) data[0] << 24) |
      ((guint32) data[1] << 16) | ((guint32) data[2] << 8) | (guint32) data[3];
}

static gboolean
write_payload_with_buffer (GOutputStream *stream, const guint8 *payload,
    gsize payload_size, GError **error)
{
  g_autofree guint8 *frame_data = NULL;
  gsize bytes_written = 0;
  gsize total_size = 0;

  total_size = WYREBOX_DAEMON_FRAME_PREFIX_BYTES + payload_size;
  frame_data = g_new (guint8, total_size);

  write_u32_be (frame_data, (guint32) payload_size);
  memcpy (frame_data + WYREBOX_DAEMON_FRAME_PREFIX_BYTES, payload,
      payload_size);

  if (!g_output_stream_write_all (stream,
          frame_data, total_size, &bytes_written, NULL, error))
    return FALSE;

  if (bytes_written != total_size) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_FAILED, "daemon frame payload write was short");
    return FALSE;
  }

  return TRUE;
}

gboolean
wyrebox_daemon_frame_io_write_payload (GOutputStream *stream,
    const guint8 *payload, gsize payload_size, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (stream == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "daemon frame writer requires a non-null output stream");
    return FALSE;
  }

  if (payload == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "daemon frame payload is required");
    return FALSE;
  }

  if (payload_size == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "daemon frame payload must be non-empty");
    return FALSE;
  }

  if (payload_size > WYREBOX_DAEMON_MAX_FRAME_SIZE_BYTES) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "daemon frame payload exceeds maximum size of %u bytes",
        WYREBOX_DAEMON_MAX_FRAME_SIZE_BYTES);
    return FALSE;
  }

  return write_payload_with_buffer (stream, payload, payload_size, error);
}

GBytes *
wyrebox_daemon_frame_io_read_payload (GInputStream *stream, GError **error)
{
  g_autoptr (GBytes) payload = NULL;
  gboolean eof = FALSE;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!wyrebox_daemon_frame_io_read_payload_or_eof (stream, &payload, &eof,
          error))
    return NULL;

  if (eof)
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "daemon frame stream ended before a frame");

  return g_steal_pointer (&payload);
}

gboolean
wyrebox_daemon_frame_io_read_payload_or_eof (GInputStream *stream,
    GBytes **out_payload, gboolean *out_eof, GError **error)
{
  guint8 prefix[WYREBOX_DAEMON_FRAME_PREFIX_BYTES] = { 0 };
  gsize frame_size = 0;
  gsize bytes_read = 0;
  g_autofree guint8 *payload = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (out_payload != NULL, FALSE);
  g_clear_pointer (out_payload, g_bytes_unref);
  if (out_eof != NULL)
    *out_eof = FALSE;

  if (stream == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "daemon frame reader requires a non-null input stream");
    return FALSE;
  }

  if (!g_input_stream_read_all (stream, prefix, sizeof (prefix), &bytes_read,
          NULL, error))
    goto read_prefix_failed;

  if (bytes_read == 0) {
    if (out_eof != NULL) {
      *out_eof = TRUE;
      return TRUE;
    }

    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "daemon frame stream ended before a frame");
    return FALSE;
  }

  if (bytes_read != sizeof (prefix)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "daemon frame length prefix is truncated");
    return FALSE;
  }

  frame_size = read_u32_be (prefix);
  if (frame_size == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "daemon frame payload must be non-empty");
    return FALSE;
  }

  if (frame_size > WYREBOX_DAEMON_MAX_FRAME_SIZE_BYTES) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "daemon frame payload size %zu exceeds maximum of %u bytes",
        frame_size, WYREBOX_DAEMON_MAX_FRAME_SIZE_BYTES);
    return FALSE;
  }

  payload = g_new (guint8, frame_size);
  if (!g_input_stream_read_all (stream, payload, frame_size, &bytes_read, NULL,
          error))
    goto read_payload_failed;

  if (bytes_read != frame_size) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "daemon frame payload is truncated");
    return FALSE;
  }

  *out_payload = g_bytes_new_take (g_steal_pointer (&payload), frame_size);
  return TRUE;

read_prefix_failed:
  if (bytes_read != 0 && bytes_read != sizeof (prefix)
      && (error == NULL || *error == NULL))
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "daemon frame length prefix is truncated");
  return FALSE;

read_payload_failed:
  if (bytes_read != frame_size && (error == NULL || *error == NULL))
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "daemon frame payload is truncated");
  return FALSE;
}
