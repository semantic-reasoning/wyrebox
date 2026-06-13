#pragma once

#include <gio/gio.h>
#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Response identity. At least one discriminator, message_id or query_id, is
   * required so callers can route or validate the chunk.
   */
  char *request_id;
  char *message_id;
  char *query_id;
  char *correlation_id;

  /*
   * Zero-based stream chunk index. The daemon preserves the caller-provided
   * index and does not infer ordering here.
   */
  guint64 chunk_index;

  /*
   * Immutable chunk payload owned by this frame. Use clear() to release it.
   */
  GBytes *bytes;
  gboolean end_of_stream;
} WyreboxDaemonStreamChunkFrame;

void wyrebox_daemon_stream_chunk_frame_clear (
    WyreboxDaemonStreamChunkFrame *frame);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonStreamChunkFrame,
    wyrebox_daemon_stream_chunk_frame_clear)

/*
 * Replaces any existing contents of @frame on success. On failure, existing
 * contents of @frame are left unchanged.
 */
gboolean wyrebox_daemon_stream_chunk_frame_init (
    WyreboxDaemonStreamChunkFrame *frame,
    const char *request_id,
    const char *message_id,
    const char *query_id,
    const char *correlation_id,
    guint64 chunk_index,
    GBytes *bytes,
    gboolean end_of_stream,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
