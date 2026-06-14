#include "wyrebox-postfix-lmtp-reply.h"

#include <string.h>

static const char *
nullable_context (const char *value)
{
  if (value == NULL || *value == '\0')
    return "<none>";

  return value;
}

const char *
wyrebox_postfix_lmtp_reply_outcome_to_string (WyreboxPostfixLmtpReplyOutcome
    outcome)
{
  switch (outcome) {
    case WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_DELIVERED:
      return "delivered";
    case WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE:
      return "temporary_failure";
    case WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_PERMANENT_FAILURE:
      return "permanent_failure";
    default:
      return NULL;
  }
}

void
wyrebox_postfix_lmtp_reply_clear (WyreboxPostfixLmtpReply *reply)
{
  if (reply == NULL)
    return;

  reply->outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE;
  reply->reply_code = 451;
  g_clear_pointer (&reply->enhanced_status, g_free);
  g_clear_pointer (&reply->reply_text, g_free);
  g_clear_pointer (&reply->log_message, g_free);
}

static void
reply_init (WyreboxPostfixLmtpReply *reply,
    WyreboxPostfixLmtpReplyOutcome outcome,
    int reply_code,
    const char *enhanced_status, const char *reply_text, char *log_message)
{
  wyrebox_postfix_lmtp_reply_clear (reply);
  reply->outcome = outcome;
  reply->reply_code = reply_code;
  reply->enhanced_status = g_strdup (enhanced_status);
  reply->reply_text = g_strdup (reply_text);
  reply->log_message = log_message;
}

static void
reply_init_temporary_failure (WyreboxPostfixLmtpReply *reply,
    const char *request_id,
    const char *correlation_id,
    const char *failure_source, const char *failure_class)
{
  reply_init (reply,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451,
      "4.3.0",
      "Temporary local delivery failure",
      g_strdup_printf ("postfix_lmtp outcome=temporary_failure "
          "reply_code=451 enhanced_status=4.3.0 request_id=%s "
          "correlation_id=%s failure_source=%s failure_class=%s",
          nullable_context (request_id),
          nullable_context (correlation_id),
          nullable_context (failure_source), nullable_context (failure_class)));
}

static gboolean
reply_init_from_success (WyreboxPostfixLmtpReply *reply,
    const WyreboxDaemonResponseFrame *response, GError **error)
{
  const WyreboxDaemonSuccessReceipt *success = &response->success;
  g_auto (WyreboxPostfixLmtpReply) next = { 0 };

  if (success->request_id == NULL || *success->request_id == '\0' ||
      success->durable_marker == NULL || *success->durable_marker == '\0' ||
      success->journal_sequence == 0) {
    reply_init_temporary_failure (&next,
        response->request_id,
        response->correlation_id, "daemon", "malformedSuccess");
    wyrebox_postfix_lmtp_reply_clear (reply);
    *reply = next;
    memset (&next, 0, sizeof next);
    return TRUE;
  }

  reply_init (&next,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_DELIVERED,
      250,
      "2.0.0",
      "Delivery accepted",
      g_strdup_printf ("postfix_lmtp outcome=delivered reply_code=250 "
          "enhanced_status=2.0.0 request_id=%s correlation_id=%s "
          "durable_marker=%s journal_offset=%" G_GUINT64_FORMAT " "
          "journal_sequence=%" G_GUINT64_FORMAT,
          nullable_context (response->request_id),
          nullable_context (response->correlation_id),
          nullable_context (success->durable_marker),
          success->journal_offset, success->journal_sequence));

  wyrebox_postfix_lmtp_reply_clear (reply);
  *reply = next;
  memset (&next, 0, sizeof next);

  return TRUE;
}

static void
reply_codes_for_daemon_error_class (WyreboxDaemonErrorClass error_class,
    WyreboxPostfixLmtpReplyOutcome *out_outcome,
    int *out_reply_code,
    const char **out_enhanced_status, const char **out_reply_text)
{
  switch (error_class) {
    case WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE:
    case WYREBOX_DAEMON_ERROR_CONFLICT:
    case WYREBOX_DAEMON_ERROR_INTERNAL_ERROR:
      *out_outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE;
      *out_reply_code = 451;
      *out_enhanced_status = "4.3.0";
      *out_reply_text = "Temporary local delivery failure";
      return;
    case WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE:
      *out_outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_PERMANENT_FAILURE;
      *out_reply_code = 554;
      *out_enhanced_status = "5.6.0";
      *out_reply_text = "Message rejected";
      return;
    case WYREBOX_DAEMON_ERROR_PERMISSION_DENIED:
      *out_outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_PERMANENT_FAILURE;
      *out_reply_code = 550;
      *out_enhanced_status = "5.7.1";
      *out_reply_text = "Delivery not authorized";
      return;
    case WYREBOX_DAEMON_ERROR_NOT_FOUND:
      *out_outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_PERMANENT_FAILURE;
      *out_reply_code = 550;
      *out_enhanced_status = "5.1.1";
      *out_reply_text = "Recipient unavailable";
      return;
    default:
      *out_outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE;
      *out_reply_code = 451;
      *out_enhanced_status = "4.3.0";
      *out_reply_text = "Temporary local delivery failure";
      return;
  }
}

static gboolean
reply_init_from_daemon_error (WyreboxPostfixLmtpReply *reply,
    const WyreboxDaemonResponseFrame *response, GError **error)
{
  const WyreboxDaemonErrorFrame *error_frame = &response->error;
  const char *error_class =
      wyrebox_daemon_error_class_to_string (error_frame->error_class);
  WyreboxPostfixLmtpReplyOutcome outcome =
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE;
  int reply_code = 451;
  const char *enhanced_status = "4.3.0";
  const char *reply_text = "Temporary local delivery failure";
  g_auto (WyreboxPostfixLmtpReply) next = { 0 };

  if (error_frame->request_id == NULL || *error_frame->request_id == '\0' ||
      error_class == NULL) {
    reply_init_temporary_failure (&next,
        response->request_id,
        response->correlation_id, "daemon", "malformedError");
    wyrebox_postfix_lmtp_reply_clear (reply);
    *reply = next;
    memset (&next, 0, sizeof next);
    return TRUE;
  }

  reply_codes_for_daemon_error_class (error_frame->error_class,
      &outcome, &reply_code, &enhanced_status, &reply_text);

  reply_init (&next,
      outcome,
      reply_code,
      enhanced_status,
      reply_text,
      g_strdup_printf ("postfix_lmtp outcome=%s reply_code=%d "
          "enhanced_status=%s request_id=%s correlation_id=%s "
          "daemon_error_class=%s",
          wyrebox_postfix_lmtp_reply_outcome_to_string (outcome),
          reply_code,
          enhanced_status,
          nullable_context (response->request_id),
          nullable_context (response->correlation_id), error_class));

  wyrebox_postfix_lmtp_reply_clear (reply);
  *reply = next;
  memset (&next, 0, sizeof next);

  return TRUE;
}

gboolean
wyrebox_postfix_lmtp_reply_init_from_response (WyreboxPostfixLmtpReply *reply,
    const WyreboxDaemonResponseFrame *response, GError **error)
{
  g_return_val_if_fail (reply != NULL, FALSE);
  g_return_val_if_fail (response != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  switch (response->kind) {
    case WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS:
      return reply_init_from_success (reply, response, error);
    case WYREBOX_DAEMON_RESPONSE_FRAME_ERROR:
      return reply_init_from_daemon_error (reply, response, error);
    case WYREBOX_DAEMON_RESPONSE_FRAME_NONE:
      reply_init_temporary_failure (reply,
          response->request_id,
          response->correlation_id, "daemon", "missingResponseKind");
      return TRUE;
    case WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK:
    case WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST:
    case WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_SELECT:
    default:
      reply_init_temporary_failure (reply,
          response->request_id,
          response->correlation_id, "daemon", "unsupportedResponseKind");
      return TRUE;
  }
}

gboolean
wyrebox_postfix_lmtp_reply_init_from_delivery_error (WyreboxPostfixLmtpReply
    *reply, const char *request_id, const char *correlation_id,
    const GError *cause, GError **error)
{
  g_auto (WyreboxPostfixLmtpReply) next = { 0 };

  g_return_val_if_fail (reply != NULL, FALSE);
  g_return_val_if_fail (cause != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  reply_init (&next,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451,
      "4.3.0",
      "Temporary local delivery failure",
      g_strdup_printf ("postfix_lmtp outcome=temporary_failure "
          "reply_code=451 enhanced_status=4.3.0 request_id=%s "
          "correlation_id=%s local_error_domain=%s local_error_code=%d",
          nullable_context (request_id),
          nullable_context (correlation_id),
          nullable_context (g_quark_to_string (cause->domain)), cause->code));

  wyrebox_postfix_lmtp_reply_clear (reply);
  *reply = next;
  memset (&next, 0, sizeof next);

  return TRUE;
}
