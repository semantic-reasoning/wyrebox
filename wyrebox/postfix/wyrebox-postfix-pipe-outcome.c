#include "wyrebox-postfix-pipe-outcome.h"

#include <sysexits.h>

static const char *
nullable_context (const char *value)
{
  if (value == NULL || *value == '\0')
    return "<none>";

  return value;
}

const char *
wyrebox_postfix_pipe_outcome_to_string (WyreboxPostfixPipeOutcome outcome)
{
  switch (outcome) {
    case WYREBOX_POSTFIX_PIPE_OUTCOME_DELIVERED:
      return "delivered";
    case WYREBOX_POSTFIX_PIPE_OUTCOME_TEMPORARY_FAILURE:
      return "temporary_failure";
    case WYREBOX_POSTFIX_PIPE_OUTCOME_PERMANENT_FAILURE:
      return "permanent_failure";
    default:
      return NULL;
  }
}

void
wyrebox_postfix_pipe_decision_clear (WyreboxPostfixPipeDecision *decision)
{
  if (decision == NULL)
    return;

  decision->outcome = WYREBOX_POSTFIX_PIPE_OUTCOME_TEMPORARY_FAILURE;
  decision->exit_status = EX_TEMPFAIL;
  g_clear_pointer (&decision->log_message, g_free);
}

static int
exit_status_for_daemon_error_class (WyreboxDaemonErrorClass error_class)
{
  switch (error_class) {
    case WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE:
    case WYREBOX_DAEMON_ERROR_CONFLICT:
    case WYREBOX_DAEMON_ERROR_INTERNAL_ERROR:
      return EX_TEMPFAIL;
    case WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE:
      return EX_DATAERR;
    case WYREBOX_DAEMON_ERROR_PERMISSION_DENIED:
      return EX_NOPERM;
    case WYREBOX_DAEMON_ERROR_NOT_FOUND:
      return EX_UNAVAILABLE;
    default:
      return EX_TEMPFAIL;
  }
}

static WyreboxPostfixPipeOutcome
outcome_for_exit_status (int exit_status)
{
  if (exit_status == EX_OK)
    return WYREBOX_POSTFIX_PIPE_OUTCOME_DELIVERED;
  if (exit_status == EX_TEMPFAIL)
    return WYREBOX_POSTFIX_PIPE_OUTCOME_TEMPORARY_FAILURE;

  return WYREBOX_POSTFIX_PIPE_OUTCOME_PERMANENT_FAILURE;
}

static void
decision_init (WyreboxPostfixPipeDecision *decision,
    WyreboxPostfixPipeOutcome outcome, int exit_status, char *log_message)
{
  wyrebox_postfix_pipe_decision_clear (decision);
  decision->outcome = outcome;
  decision->exit_status = exit_status;
  decision->log_message = log_message;
}

static gboolean
decision_init_from_success (WyreboxPostfixPipeDecision *decision,
    const WyreboxDaemonResponseFrame *response, GError **error)
{
  const WyreboxDaemonSuccessReceipt *success = &response->success;
  g_auto (WyreboxPostfixPipeDecision) next = { 0 };

  if (success->request_id == NULL || *success->request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "success response missing request_id");
    return FALSE;
  }

  decision_init (&next,
      WYREBOX_POSTFIX_PIPE_OUTCOME_DELIVERED,
      EX_OK,
      g_strdup_printf ("postfix_pipe outcome=delivered exit_status=%d "
          "request_id=%s correlation_id=%s durable_marker=%s "
          "journal_offset=%" G_GUINT64_FORMAT " journal_sequence=%"
          G_GUINT64_FORMAT,
          EX_OK,
          nullable_context (response->request_id),
          nullable_context (response->correlation_id),
          nullable_context (success->durable_marker),
          success->journal_offset, success->journal_sequence));

  wyrebox_postfix_pipe_decision_clear (decision);
  *decision = next;
  next.log_message = NULL;

  return TRUE;
}

static gboolean
decision_init_from_daemon_error (WyreboxPostfixPipeDecision *decision,
    const WyreboxDaemonResponseFrame *response, GError **error)
{
  const WyreboxDaemonErrorFrame *error_frame = &response->error;
  const char *error_class =
      wyrebox_daemon_error_class_to_string (error_frame->error_class);
  int exit_status = EX_TEMPFAIL;
  WyreboxPostfixPipeOutcome outcome =
      WYREBOX_POSTFIX_PIPE_OUTCOME_TEMPORARY_FAILURE;
  g_auto (WyreboxPostfixPipeDecision) next = { 0 };

  if (error_frame->request_id == NULL || *error_frame->request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "error response missing request_id");
    return FALSE;
  }

  if (error_class == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "error response has unsupported error class");
    return FALSE;
  }

  exit_status = exit_status_for_daemon_error_class (error_frame->error_class);
  outcome = outcome_for_exit_status (exit_status);

  decision_init (&next,
      outcome,
      exit_status,
      g_strdup_printf ("postfix_pipe outcome=%s exit_status=%d "
          "request_id=%s correlation_id=%s daemon_error_class=%s",
          wyrebox_postfix_pipe_outcome_to_string (outcome),
          exit_status,
          nullable_context (response->request_id),
          nullable_context (response->correlation_id), error_class));

  wyrebox_postfix_pipe_decision_clear (decision);
  *decision = next;
  next.log_message = NULL;

  return TRUE;
}

gboolean
wyrebox_postfix_pipe_decision_init_from_response (WyreboxPostfixPipeDecision
    *decision, const WyreboxDaemonResponseFrame *response, GError **error)
{
  g_return_val_if_fail (decision != NULL, FALSE);
  g_return_val_if_fail (response != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  switch (response->kind) {
    case WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS:
      return decision_init_from_success (decision, response, error);
    case WYREBOX_DAEMON_RESPONSE_FRAME_ERROR:
      return decision_init_from_daemon_error (decision, response, error);
    case WYREBOX_DAEMON_RESPONSE_FRAME_NONE:
      g_set_error (error,
          G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "missing daemon response kind");
      return FALSE;
    case WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK:
    case WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST:
    case WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_SELECT:
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "unsupported daemon response kind for Postfix pipe delivery");
      return FALSE;
  }
}

gboolean
wyrebox_postfix_pipe_decision_init_from_local_error (WyreboxPostfixPipeDecision
    *decision, const char *request_id, const char *correlation_id,
    const GError *cause, GError **error)
{
  g_auto (WyreboxPostfixPipeDecision) next = { 0 };

  g_return_val_if_fail (decision != NULL, FALSE);
  g_return_val_if_fail (cause != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  decision_init (&next,
      WYREBOX_POSTFIX_PIPE_OUTCOME_TEMPORARY_FAILURE,
      EX_TEMPFAIL,
      g_strdup_printf ("postfix_pipe outcome=temporary_failure "
          "exit_status=%d request_id=%s correlation_id=%s "
          "local_error_domain=%s local_error_code=%d",
          EX_TEMPFAIL,
          nullable_context (request_id),
          nullable_context (correlation_id),
          nullable_context (g_quark_to_string (cause->domain)), cause->code));

  wyrebox_postfix_pipe_decision_clear (decision);
  *decision = next;
  next.log_message = NULL;

  return TRUE;
}
