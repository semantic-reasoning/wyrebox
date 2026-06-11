#pragma once

#include "wyrebox-daemon-error.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Request identity copied into the daemon error frame.
   *
   * Ownership: caller owns and must clear with
   * wyrebox_daemon_error_frame_clear().
   */
  char *request_id;

  WyreboxDaemonErrorClass error_class;

  /*
   * Human-readable error message and optional retry hint for callers/logs.
   */
  char *message;
  char *retry_hint;
} WyreboxDaemonErrorFrame;

void wyrebox_daemon_error_frame_clear (WyreboxDaemonErrorFrame *frame);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonErrorFrame,
    wyrebox_daemon_error_frame_clear)

gboolean wyrebox_daemon_error_frame_init (WyreboxDaemonErrorFrame *frame,
    const char *request_id,
    WyreboxDaemonErrorClass error_class,
    const char *message,
    const char *retry_hint,
    GError **error);

gboolean wyrebox_daemon_error_frame_init_from_g_error (
    WyreboxDaemonErrorFrame *frame,
    const char *request_id,
    const GError *cause,
    const char *retry_hint,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
