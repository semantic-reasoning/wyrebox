#include "wyrebox-postfix-pipe-outcome.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>
#include <sysexits.h>

typedef struct
{
  WyreboxDaemonErrorClass error_class;
  WyreboxPostfixPipeOutcome expected_outcome;
  int expected_exit_status;
  const char *expected_error_class;
} DaemonErrorCase;

static WyreboxDaemonSuccessReceipt
make_success_receipt (void)
{
  WyreboxDaemonSuccessReceipt receipt = {
    .request_id = g_strdup ("request-success"),
    .durable_marker = g_strdup ("journal:12:34"),
    .journal_offset = 12,
    .journal_sequence = 34,
    .summary = g_strdup ("raw-message-bytes-must-not-appear"),
  };

  return receipt;
}

static void
assert_log_contains_common_context (const WyreboxPostfixPipeDecision *decision,
    const char *outcome,
    int exit_status, const char *request_id, const char *correlation_id)
{
  g_autofree char *exit_context = g_strdup_printf ("exit_status=%d",
      exit_status);
  g_autofree char *request_context = g_strdup_printf ("request_id=%s",
      request_id);
  g_autofree char *correlation_context =
      g_strdup_printf ("correlation_id=%s", correlation_id);

  g_assert_nonnull (decision->log_message);
  g_assert_nonnull (strstr (decision->log_message, outcome));
  g_assert_nonnull (strstr (decision->log_message, exit_context));
  g_assert_nonnull (strstr (decision->log_message, request_context));
  g_assert_nonnull (strstr (decision->log_message, correlation_context));
}

static void
test_success_maps_to_delivered_ex_ok (void)
{
  g_auto (WyreboxDaemonSuccessReceipt) receipt = make_success_receipt ();
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxPostfixPipeDecision) decision = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_response_frame_init_success (&response,
          &receipt, "correlation-success", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_postfix_pipe_decision_init_from_response (&decision,
          &response, &error));
  g_assert_no_error (error);

  g_assert_cmpint (decision.outcome, ==,
      WYREBOX_POSTFIX_PIPE_OUTCOME_DELIVERED);
  g_assert_cmpint (decision.exit_status, ==, EX_OK);
  assert_log_contains_common_context (&decision,
      "outcome=delivered", EX_OK, "request-success", "correlation-success");
  g_assert_nonnull (strstr (decision.log_message,
          "durable_marker=journal:12:34"));
  g_assert_nonnull (strstr (decision.log_message, "journal_offset=12"));
  g_assert_nonnull (strstr (decision.log_message, "journal_sequence=34"));
  g_assert_null (strstr (decision.log_message,
          "raw-message-bytes-must-not-appear"));
}

static void
assert_daemon_error_maps (const DaemonErrorCase *test_case)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxPostfixPipeDecision) decision = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_error_frame_init (&error_frame,
          "request-error",
          test_case->error_class,
          "raw-message-bytes-must-not-appear", "retry later", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&response,
          &error_frame, "correlation-error", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_postfix_pipe_decision_init_from_response (&decision,
          &response, &error));
  g_assert_no_error (error);

  g_assert_cmpint (decision.outcome, ==, test_case->expected_outcome);
  g_assert_cmpint (decision.exit_status, ==, test_case->expected_exit_status);
  assert_log_contains_common_context (&decision,
      wyrebox_postfix_pipe_outcome_to_string (test_case->expected_outcome),
      test_case->expected_exit_status, "request-error", "correlation-error");
  g_assert_nonnull (strstr (decision.log_message,
          test_case->expected_error_class));
  g_assert_null (strstr (decision.log_message,
          "raw-message-bytes-must-not-appear"));
  g_assert_null (strstr (decision.log_message, "retry later"));
}

static void
test_daemon_temporary_failure_maps_to_tempfail (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE,
    .expected_outcome = WYREBOX_POSTFIX_PIPE_OUTCOME_TEMPORARY_FAILURE,
    .expected_exit_status = EX_TEMPFAIL,
    .expected_error_class = "temporaryFailure",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_daemon_permanent_failure_maps_to_dataerr (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE,
    .expected_outcome = WYREBOX_POSTFIX_PIPE_OUTCOME_PERMANENT_FAILURE,
    .expected_exit_status = EX_DATAERR,
    .expected_error_class = "permanentFailure",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_daemon_permission_denied_maps_to_noperm (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_PERMISSION_DENIED,
    .expected_outcome = WYREBOX_POSTFIX_PIPE_OUTCOME_PERMANENT_FAILURE,
    .expected_exit_status = EX_NOPERM,
    .expected_error_class = "permissionDenied",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_daemon_not_found_maps_to_unavailable (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_NOT_FOUND,
    .expected_outcome = WYREBOX_POSTFIX_PIPE_OUTCOME_PERMANENT_FAILURE,
    .expected_exit_status = EX_UNAVAILABLE,
    .expected_error_class = "notFound",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_daemon_conflict_maps_to_tempfail (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_CONFLICT,
    .expected_outcome = WYREBOX_POSTFIX_PIPE_OUTCOME_TEMPORARY_FAILURE,
    .expected_exit_status = EX_TEMPFAIL,
    .expected_error_class = "conflict",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_daemon_internal_error_maps_to_tempfail (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
    .expected_outcome = WYREBOX_POSTFIX_PIPE_OUTCOME_TEMPORARY_FAILURE,
    .expected_exit_status = EX_TEMPFAIL,
    .expected_error_class = "internalError",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_local_error_maps_to_temporary_failure (void)
{
  g_auto (WyreboxPostfixPipeDecision) decision = { 0 };
  g_autoptr (GError) cause = g_error_new (G_IO_ERROR,
      G_IO_ERROR_CONNECTION_REFUSED, "raw-message-bytes-must-not-appear");
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_postfix_pipe_decision_init_from_local_error (&decision,
          "request-local", "correlation-local", cause, &error));
  g_assert_no_error (error);

  g_assert_cmpint (decision.outcome, ==,
      WYREBOX_POSTFIX_PIPE_OUTCOME_TEMPORARY_FAILURE);
  g_assert_cmpint (decision.exit_status, ==, EX_TEMPFAIL);
  assert_log_contains_common_context (&decision,
      "outcome=temporary_failure",
      EX_TEMPFAIL, "request-local", "correlation-local");
  g_assert_nonnull (strstr (decision.log_message, "local_error_domain="));
  g_assert_nonnull (strstr (decision.log_message, "local_error_code="));
  g_assert_null (strstr (decision.log_message,
          "raw-message-bytes-must-not-appear"));
}

static void
test_missing_response_kind_fails_invalid_data (void)
{
  WyreboxDaemonResponseFrame response = { 0 };
  g_auto (WyreboxPostfixPipeDecision) decision = {
    .outcome = WYREBOX_POSTFIX_PIPE_OUTCOME_DELIVERED,
    .exit_status = EX_OK,
    .log_message = g_strdup ("keep-existing"),
  };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_postfix_pipe_decision_init_from_response (&decision,
          &response, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpint (decision.outcome, ==,
      WYREBOX_POSTFIX_PIPE_OUTCOME_DELIVERED);
  g_assert_cmpint (decision.exit_status, ==, EX_OK);
  g_assert_cmpstr (decision.log_message, ==, "keep-existing");
}

static void
test_unsupported_response_kind_fails_invalid_data (void)
{
  WyreboxDaemonResponseFrame response = {
    .request_id = "request-stream",
    .correlation_id = "correlation-stream",
    .kind = WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK,
  };
  g_auto (WyreboxPostfixPipeDecision) decision = {
    .outcome = WYREBOX_POSTFIX_PIPE_OUTCOME_DELIVERED,
    .exit_status = EX_OK,
    .log_message = g_strdup ("keep-existing"),
  };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_postfix_pipe_decision_init_from_response (&decision,
          &response, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpint (decision.outcome, ==,
      WYREBOX_POSTFIX_PIPE_OUTCOME_DELIVERED);
  g_assert_cmpint (decision.exit_status, ==, EX_OK);
  g_assert_cmpstr (decision.log_message, ==, "keep-existing");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/postfix/pipe-outcome/success-maps-to-delivered-ex-ok",
      test_success_maps_to_delivered_ex_ok);
  g_test_add_func ("/postfix/pipe-outcome/temporary-failure-maps-to-tempfail",
      test_daemon_temporary_failure_maps_to_tempfail);
  g_test_add_func ("/postfix/pipe-outcome/permanent-failure-maps-to-dataerr",
      test_daemon_permanent_failure_maps_to_dataerr);
  g_test_add_func ("/postfix/pipe-outcome/permission-denied-maps-to-noperm",
      test_daemon_permission_denied_maps_to_noperm);
  g_test_add_func ("/postfix/pipe-outcome/not-found-maps-to-unavailable",
      test_daemon_not_found_maps_to_unavailable);
  g_test_add_func ("/postfix/pipe-outcome/conflict-maps-to-tempfail",
      test_daemon_conflict_maps_to_tempfail);
  g_test_add_func ("/postfix/pipe-outcome/internal-error-maps-to-tempfail",
      test_daemon_internal_error_maps_to_tempfail);
  g_test_add_func ("/postfix/pipe-outcome/local-error-maps-to-tempfail",
      test_local_error_maps_to_temporary_failure);
  g_test_add_func ("/postfix/pipe-outcome/missing-kind-fails-invalid-data",
      test_missing_response_kind_fails_invalid_data);
  g_test_add_func ("/postfix/pipe-outcome/unsupported-kind-fails-invalid-data",
      test_unsupported_response_kind_fails_invalid_data);

  return g_test_run ();
}
