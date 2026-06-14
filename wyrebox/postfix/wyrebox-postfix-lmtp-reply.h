#pragma once

#include "wyrebox-daemon-response-frame.h"

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum {
  WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_DELIVERED,
  WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
  WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_PERMANENT_FAILURE,
} WyreboxPostfixLmtpReplyOutcome;

typedef struct
{
  WyreboxPostfixLmtpReplyOutcome outcome;
  int reply_code;
  char *enhanced_status;
  char *reply_text;

  /*
   * Log-safe operational context. It is built from request identity, daemon
   * result class, and durable journal metadata, never raw message bytes or
   * daemon/local free-form error text.
   */
  char *log_message;
} WyreboxPostfixLmtpReply;

const char *wyrebox_postfix_lmtp_reply_outcome_to_string (
    WyreboxPostfixLmtpReplyOutcome outcome);

void wyrebox_postfix_lmtp_reply_clear (
    WyreboxPostfixLmtpReply *reply);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxPostfixLmtpReply,
    wyrebox_postfix_lmtp_reply_clear)

gboolean wyrebox_postfix_lmtp_reply_init_from_response (
    WyreboxPostfixLmtpReply *reply,
    const WyreboxDaemonResponseFrame *response,
    GError **error);

gboolean wyrebox_postfix_lmtp_reply_init_from_delivery_error (
    WyreboxPostfixLmtpReply *reply,
    const char *request_id,
    const char *correlation_id,
    const GError *cause,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
