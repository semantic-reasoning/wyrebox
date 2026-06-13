#pragma once

#include "wyrebox-daemon-response-frame.h"

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum {
  WYREBOX_POSTFIX_PIPE_OUTCOME_DELIVERED,
  WYREBOX_POSTFIX_PIPE_OUTCOME_TEMPORARY_FAILURE,
  WYREBOX_POSTFIX_PIPE_OUTCOME_PERMANENT_FAILURE,
} WyreboxPostfixPipeOutcome;

typedef struct
{
  WyreboxPostfixPipeOutcome outcome;
  int exit_status;

  /*
   * Log-safe operational context. This is built from request identity,
   * daemon result class, and durable journal metadata, never raw message
   * bytes or daemon/local free-form error text.
   */
  char *log_message;
} WyreboxPostfixPipeDecision;

const char *wyrebox_postfix_pipe_outcome_to_string (
    WyreboxPostfixPipeOutcome outcome);

void wyrebox_postfix_pipe_decision_clear (
    WyreboxPostfixPipeDecision *decision);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxPostfixPipeDecision,
    wyrebox_postfix_pipe_decision_clear)

/*
 * Reinitialization semantics for init helpers:
 * - on success, replaces any existing contents of @decision;
 * - on failure, leaves any existing contents of @decision unchanged.
 */
gboolean wyrebox_postfix_pipe_decision_init_from_response (
    WyreboxPostfixPipeDecision *decision,
    const WyreboxDaemonResponseFrame *response,
    GError **error);

gboolean wyrebox_postfix_pipe_decision_init_from_local_error (
    WyreboxPostfixPipeDecision *decision,
    const char *request_id,
    const char *correlation_id,
    const GError *cause,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
