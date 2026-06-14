#include "wyrebox-postfix-lmtp-reply.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

typedef struct
{
  WyreboxDaemonErrorClass error_class;
  WyreboxPostfixLmtpReplyOutcome expected_outcome;
  int expected_reply_code;
  const char *expected_enhanced_status;
  const char *expected_reply_text;
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
assert_log_contains_common_context (const WyreboxPostfixLmtpReply *reply,
    const char *outcome,
    int reply_code, const char *enhanced_status, const char *request_id,
    const char *correlation_id)
{
  g_autofree char *reply_code_context =
      g_strdup_printf ("reply_code=%d", reply_code);
  g_autofree char *enhanced_status_context =
      g_strdup_printf ("enhanced_status=%s", enhanced_status);
  g_autofree char *request_context = g_strdup_printf ("request_id=%s",
      request_id);
  g_autofree char *correlation_context =
      g_strdup_printf ("correlation_id=%s", correlation_id);

  g_assert_nonnull (reply->log_message);
  g_assert_nonnull (strstr (reply->log_message, outcome));
  g_assert_nonnull (strstr (reply->log_message, reply_code_context));
  g_assert_nonnull (strstr (reply->log_message, enhanced_status_context));
  g_assert_nonnull (strstr (reply->log_message, request_context));
  g_assert_nonnull (strstr (reply->log_message, correlation_context));
}

static void
assert_reply_values (const WyreboxPostfixLmtpReply *reply,
    WyreboxPostfixLmtpReplyOutcome outcome,
    int reply_code, const char *enhanced_status, const char *reply_text)
{
  g_assert_cmpint (reply->outcome, ==, outcome);
  g_assert_cmpint (reply->reply_code, ==, reply_code);
  g_assert_cmpstr (reply->enhanced_status, ==, enhanced_status);
  g_assert_cmpstr (reply->reply_text, ==, reply_text);
}

static void
assert_reply_has_no_unsafe_text (const WyreboxPostfixLmtpReply *reply)
{
  g_assert_null (strstr (reply->reply_text,
          "raw-message-bytes-must-not-appear"));
  g_assert_null (strstr (reply->reply_text, "retry later"));
  g_assert_null (strstr (reply->log_message,
          "raw-message-bytes-must-not-appear"));
  g_assert_null (strstr (reply->log_message, "retry later"));
}

static void
test_success_maps_to_250_after_durable_receipt (void)
{
  g_auto (WyreboxDaemonSuccessReceipt) receipt = make_success_receipt ();
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxPostfixLmtpReply) reply = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_response_frame_init_success (&response,
          &receipt, "correlation-success", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_postfix_lmtp_reply_init_from_response (&reply,
          &response, &error));
  g_assert_no_error (error);

  assert_reply_values (&reply,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_DELIVERED,
      250, "2.0.0", "Delivery accepted");
  assert_log_contains_common_context (&reply,
      "outcome=delivered",
      250, "2.0.0", "request-success", "correlation-success");
  g_assert_nonnull (strstr (reply.log_message, "durable_marker=journal:12:34"));
  g_assert_nonnull (strstr (reply.log_message, "journal_offset=12"));
  g_assert_nonnull (strstr (reply.log_message, "journal_sequence=34"));
  assert_reply_has_no_unsafe_text (&reply);
}

static void
assert_daemon_error_maps (const DaemonErrorCase *test_case)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxPostfixLmtpReply) reply = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_error_frame_init (&error_frame,
          "request-error",
          test_case->error_class,
          "raw-message-bytes-must-not-appear", "retry later", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&response,
          &error_frame, "correlation-error", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_postfix_lmtp_reply_init_from_response (&reply,
          &response, &error));
  g_assert_no_error (error);

  assert_reply_values (&reply,
      test_case->expected_outcome,
      test_case->expected_reply_code,
      test_case->expected_enhanced_status, test_case->expected_reply_text);
  assert_log_contains_common_context (&reply,
      wyrebox_postfix_lmtp_reply_outcome_to_string
      (test_case->expected_outcome),
      test_case->expected_reply_code,
      test_case->expected_enhanced_status,
      "request-error", "correlation-error");
  g_assert_nonnull (strstr (reply.log_message,
          test_case->expected_error_class));
  assert_reply_has_no_unsafe_text (&reply);
}

static void
test_daemon_temporary_failure_maps_to_451 (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE,
    .expected_outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
    .expected_reply_code = 451,
    .expected_enhanced_status = "4.3.0",
    .expected_reply_text = "Temporary local delivery failure",
    .expected_error_class = "temporaryFailure",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_daemon_permanent_failure_maps_to_554 (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE,
    .expected_outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_PERMANENT_FAILURE,
    .expected_reply_code = 554,
    .expected_enhanced_status = "5.6.0",
    .expected_reply_text = "Message rejected",
    .expected_error_class = "permanentFailure",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_daemon_permission_denied_maps_to_550_571 (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_PERMISSION_DENIED,
    .expected_outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_PERMANENT_FAILURE,
    .expected_reply_code = 550,
    .expected_enhanced_status = "5.7.1",
    .expected_reply_text = "Delivery not authorized",
    .expected_error_class = "permissionDenied",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_daemon_not_found_maps_to_550_511 (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_NOT_FOUND,
    .expected_outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_PERMANENT_FAILURE,
    .expected_reply_code = 550,
    .expected_enhanced_status = "5.1.1",
    .expected_reply_text = "Recipient unavailable",
    .expected_error_class = "notFound",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_daemon_conflict_maps_to_451 (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_CONFLICT,
    .expected_outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
    .expected_reply_code = 451,
    .expected_enhanced_status = "4.3.0",
    .expected_reply_text = "Temporary local delivery failure",
    .expected_error_class = "conflict",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_daemon_internal_error_maps_to_451 (void)
{
  const DaemonErrorCase test_case = {
    .error_class = WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
    .expected_outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
    .expected_reply_code = 451,
    .expected_enhanced_status = "4.3.0",
    .expected_reply_text = "Temporary local delivery failure",
    .expected_error_class = "internalError",
  };

  assert_daemon_error_maps (&test_case);
}

static void
test_local_delivery_error_maps_to_temporary_failure (void)
{
  g_auto (WyreboxPostfixLmtpReply) reply = { 0 };
  g_autoptr (GError) cause = g_error_new (G_IO_ERROR,
      G_IO_ERROR_CONNECTION_REFUSED, "raw-message-bytes-must-not-appear");
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_postfix_lmtp_reply_init_from_delivery_error (&reply,
          "request-local", "correlation-local", cause, &error));
  g_assert_no_error (error);

  assert_reply_values (&reply,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451, "4.3.0", "Temporary local delivery failure");
  assert_log_contains_common_context (&reply,
      "outcome=temporary_failure",
      451, "4.3.0", "request-local", "correlation-local");
  g_assert_nonnull (strstr (reply.log_message, "local_error_domain="));
  g_assert_nonnull (strstr (reply.log_message, "local_error_code="));
  assert_reply_has_no_unsafe_text (&reply);
}

static void
test_missing_response_kind_maps_to_temporary_failure (void)
{
  WyreboxDaemonResponseFrame response = {
    .request_id = "request-missing-kind",
    .correlation_id = "correlation-missing-kind",
  };
  g_auto (WyreboxPostfixLmtpReply) reply = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_postfix_lmtp_reply_init_from_response (&reply,
          &response, &error));
  g_assert_no_error (error);

  assert_reply_values (&reply,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451, "4.3.0", "Temporary local delivery failure");
  g_assert_nonnull (strstr (reply.log_message, "missingResponseKind"));
}

static void
test_unsupported_response_kind_maps_to_temporary_failure (void)
{
  WyreboxDaemonResponseFrame response = {
    .request_id = "request-stream",
    .correlation_id = "correlation-stream",
    .kind = WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK,
  };
  g_auto (WyreboxPostfixLmtpReply) reply = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_postfix_lmtp_reply_init_from_response (&reply,
          &response, &error));
  g_assert_no_error (error);

  assert_reply_values (&reply,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451, "4.3.0", "Temporary local delivery failure");
  g_assert_nonnull (strstr (reply.log_message, "unsupportedResponseKind"));
}

static void
test_missing_success_request_id_maps_to_temporary_failure (void)
{
  WyreboxDaemonSuccessReceipt receipt = {
    .durable_marker = g_strdup ("journal:12:34"),
    .journal_offset = 12,
    .journal_sequence = 34,
    .summary = g_strdup ("raw-message-bytes-must-not-appear"),
  };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxPostfixLmtpReply) reply = {
    .outcome = WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_DELIVERED,
    .reply_code = 250,
    .enhanced_status = g_strdup ("2.0.0"),
    .reply_text = g_strdup ("keep-old-reply"),
    .log_message = g_strdup ("keep-old-log"),
  };
  g_autoptr (GError) error = NULL;

  response.kind = WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS;
  response.correlation_id = g_strdup ("correlation-malformed");
  response.success = receipt;
  memset (&receipt, 0, sizeof receipt);

  g_assert_true (wyrebox_postfix_lmtp_reply_init_from_response (&reply,
          &response, &error));
  g_assert_no_error (error);

  assert_reply_values (&reply,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451, "4.3.0", "Temporary local delivery failure");
  g_assert_nonnull (strstr (reply.log_message, "malformedSuccess"));
  assert_reply_has_no_unsafe_text (&reply);
}

static void
test_missing_success_durable_marker_maps_to_temporary_failure (void)
{
  WyreboxDaemonSuccessReceipt receipt = {
    .request_id = g_strdup ("request-malformed"),
    .journal_offset = 12,
    .journal_sequence = 34,
    .summary = g_strdup ("raw-message-bytes-must-not-appear"),
  };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxPostfixLmtpReply) reply = { 0 };
  g_autoptr (GError) error = NULL;

  response.kind = WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS;
  response.request_id = g_strdup ("request-malformed");
  response.correlation_id = g_strdup ("correlation-malformed");
  response.success = receipt;
  memset (&receipt, 0, sizeof receipt);

  g_assert_true (wyrebox_postfix_lmtp_reply_init_from_response (&reply,
          &response, &error));
  g_assert_no_error (error);

  assert_reply_values (&reply,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451, "4.3.0", "Temporary local delivery failure");
  g_assert_nonnull (strstr (reply.log_message, "malformedSuccess"));
  assert_reply_has_no_unsafe_text (&reply);
}

static void
test_zero_success_journal_sequence_maps_to_temporary_failure (void)
{
  WyreboxDaemonSuccessReceipt receipt = {
    .request_id = g_strdup ("request-malformed"),
    .durable_marker = g_strdup ("journal:12:0"),
    .journal_offset = 12,
    .summary = g_strdup ("raw-message-bytes-must-not-appear"),
  };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxPostfixLmtpReply) reply = { 0 };
  g_autoptr (GError) error = NULL;

  response.kind = WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS;
  response.request_id = g_strdup ("request-malformed");
  response.correlation_id = g_strdup ("correlation-malformed");
  response.success = receipt;
  memset (&receipt, 0, sizeof receipt);

  g_assert_true (wyrebox_postfix_lmtp_reply_init_from_response (&reply,
          &response, &error));
  g_assert_no_error (error);

  assert_reply_values (&reply,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451, "4.3.0", "Temporary local delivery failure");
  g_assert_nonnull (strstr (reply.log_message, "malformedSuccess"));
  assert_reply_has_no_unsafe_text (&reply);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/postfix/lmtp-reply/success-maps-to-250",
      test_success_maps_to_250_after_durable_receipt);
  g_test_add_func ("/postfix/lmtp-reply/temporary-failure-maps-to-451",
      test_daemon_temporary_failure_maps_to_451);
  g_test_add_func ("/postfix/lmtp-reply/permanent-failure-maps-to-554",
      test_daemon_permanent_failure_maps_to_554);
  g_test_add_func ("/postfix/lmtp-reply/permission-denied-maps-to-550-571",
      test_daemon_permission_denied_maps_to_550_571);
  g_test_add_func ("/postfix/lmtp-reply/not-found-maps-to-550-511",
      test_daemon_not_found_maps_to_550_511);
  g_test_add_func ("/postfix/lmtp-reply/conflict-maps-to-451",
      test_daemon_conflict_maps_to_451);
  g_test_add_func ("/postfix/lmtp-reply/internal-error-maps-to-451",
      test_daemon_internal_error_maps_to_451);
  g_test_add_func ("/postfix/lmtp-reply/local-error-maps-to-451",
      test_local_delivery_error_maps_to_temporary_failure);
  g_test_add_func ("/postfix/lmtp-reply/missing-kind-maps-to-451",
      test_missing_response_kind_maps_to_temporary_failure);
  g_test_add_func ("/postfix/lmtp-reply/unsupported-kind-maps-to-451",
      test_unsupported_response_kind_maps_to_temporary_failure);
  g_test_add_func ("/postfix/lmtp-reply/missing-success-request-id",
      test_missing_success_request_id_maps_to_temporary_failure);
  g_test_add_func ("/postfix/lmtp-reply/missing-success-durable-marker",
      test_missing_success_durable_marker_maps_to_temporary_failure);
  g_test_add_func ("/postfix/lmtp-reply/zero-success-journal-sequence",
      test_zero_success_journal_sequence_maps_to_temporary_failure);

  return g_test_run ();
}
