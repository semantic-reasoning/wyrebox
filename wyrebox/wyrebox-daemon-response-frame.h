#pragma once

#include "wyrebox-daemon-error-frame.h"
#include "wyrebox-daemon-success-receipt.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum {
  WYREBOX_DAEMON_RESPONSE_FRAME_NONE,
  WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS,
  WYREBOX_DAEMON_RESPONSE_FRAME_ERROR,
} WyreboxDaemonResponseFrameKind;

typedef struct
{
  /*
   * Response envelope identity copied from the active payload.
   */
  char *request_id;
  char *correlation_id;

  /*
   * Exactly one payload is active when kind is SUCCESS or ERROR.
   */
  WyreboxDaemonResponseFrameKind kind;
  WyreboxDaemonSuccessReceipt success;
  WyreboxDaemonErrorFrame error;
} WyreboxDaemonResponseFrame;

void wyrebox_daemon_response_frame_clear (WyreboxDaemonResponseFrame *frame);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonResponseFrame,
    wyrebox_daemon_response_frame_clear)

/*
 * Reinitialization semantics for init helpers:
 * - on success, replaces any existing contents of @frame;
 * - on failure, leaves any existing contents of @frame unchanged.
 */
gboolean wyrebox_daemon_response_frame_init_success (
    WyreboxDaemonResponseFrame *frame,
    const WyreboxDaemonSuccessReceipt *success,
    const char *correlation_id,
    GError **error);

gboolean wyrebox_daemon_response_frame_init_error (
    WyreboxDaemonResponseFrame *frame,
    const WyreboxDaemonErrorFrame *error_frame,
    const char *correlation_id,
    GError **error);

gboolean wyrebox_daemon_response_frame_init_fact_mutation_success (
    WyreboxDaemonResponseFrame *frame,
    const char *request_id,
    const char *correlation_id,
    const WyreboxDaemonFactMutationRequest *request,
    guint64 journal_offset,
    guint64 journal_sequence,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
