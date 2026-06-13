#pragma once

#include "wyrebox-daemon-error-frame.h"
#include "wyrebox-daemon-mailbox-list-result.h"
#include "wyrebox-daemon-mailbox-select-result.h"
#include "wyrebox-daemon-stream-chunk-frame.h"
#include "wyrebox-daemon-success-receipt.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum {
  WYREBOX_DAEMON_RESPONSE_FRAME_NONE,
  WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS,
  WYREBOX_DAEMON_RESPONSE_FRAME_ERROR,
  WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK,
  WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST,
  WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_SELECT,
} WyreboxDaemonResponseFrameKind;

typedef struct
{
  /*
   * Response envelope identity copied from the active payload.
   */
  char *request_id;
  char *correlation_id;

  /*
   * Exactly one payload is active when kind is not NONE.
   */
  WyreboxDaemonResponseFrameKind kind;
  WyreboxDaemonSuccessReceipt success;
  WyreboxDaemonErrorFrame error;
  WyreboxDaemonStreamChunkFrame stream_chunk;
  WyreboxDaemonMailboxListResult mailbox_list;
  WyreboxDaemonMailboxSelectResult mailbox_select;
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

gboolean wyrebox_daemon_response_frame_init_stream_chunk (
    WyreboxDaemonResponseFrame *frame,
    const WyreboxDaemonStreamChunkFrame *stream_chunk,
    GError **error);

gboolean wyrebox_daemon_response_frame_init_mailbox_list (
    WyreboxDaemonResponseFrame *frame,
    const char *request_id,
    const char *correlation_id,
    const WyreboxDaemonMailboxListResult *mailbox_list,
    GError **error);

gboolean wyrebox_daemon_response_frame_init_mailbox_select (
    WyreboxDaemonResponseFrame *frame,
    const char *request_id,
    const char *correlation_id,
    const WyreboxDaemonMailboxSelectResult *mailbox_select,
    GError **error);

gboolean wyrebox_daemon_response_frame_init_fact_mutation_success (
    WyreboxDaemonResponseFrame *frame,
    const char *request_id,
    const char *correlation_id,
    const WyreboxDaemonFactMutationRequest *request,
    guint64 journal_offset,
    guint64 journal_sequence,
    GError **error);

gboolean wyrebox_daemon_response_frame_init_fact_batch_import_success (
    WyreboxDaemonResponseFrame *frame,
    const char *request_id,
    const char *correlation_id,
    const WyreboxDaemonFactBatchImportRequest *request,
    guint64 journal_offset,
    guint64 journal_sequence,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
