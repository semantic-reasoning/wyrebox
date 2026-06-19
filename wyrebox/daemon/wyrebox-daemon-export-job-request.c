#include "wyrebox-daemon-export-job-request.h"

#include <gio/gio.h>

static gboolean
validate_required_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "export job request %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "export job request %s must not contain control characters",
          field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_cursor (guint64 after_journal_offset, guint64 after_journal_sequence,
    GError **error)
{
  if (after_journal_offset != 0 && after_journal_sequence == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "export job request requires a non-zero afterJournalSequence when "
        "afterJournalOffset is non-zero");
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_time_range (guint64 start_unix_us, guint64 end_unix_us, GError **error)
{
  if (start_unix_us != 0 && end_unix_us != 0 && start_unix_us > end_unix_us) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "export job request start_unix_us must not be greater than end_unix_us");
    return FALSE;
  }

  return TRUE;
}

void
wyrebox_daemon_export_job_request_clear (WyreboxDaemonExportJobRequest *request)
{
  if (request == NULL)
    return;

  g_clear_pointer (&request->account_identity, g_free);
  g_clear_pointer (&request->dataset_id, g_free);
  g_clear_pointer (&request->output_format, g_free);
  request->after_journal_offset = 0;
  request->after_journal_sequence = 0;
  request->start_unix_us = 0;
  request->end_unix_us = 0;
}

gboolean
wyrebox_daemon_export_job_request_init (WyreboxDaemonExportJobRequest *request,
    const char *account_identity, const char *dataset_id,
    const char *output_format, guint64 after_journal_offset,
    guint64 after_journal_sequence, guint64 start_unix_us,
    guint64 end_unix_us, GError **error)
{
  g_auto (WyreboxDaemonExportJobRequest) next = { 0 };

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_text (account_identity, "account_identity", error) ||
      !validate_required_text (dataset_id, "dataset_id", error) ||
      !validate_required_text (output_format, "output_format", error) ||
      !validate_cursor (after_journal_offset, after_journal_sequence, error) ||
      !validate_time_range (start_unix_us, end_unix_us, error))
    return FALSE;

  next.account_identity = g_strdup (account_identity);
  next.dataset_id = g_strdup (dataset_id);
  next.output_format = g_strdup (output_format);
  next.after_journal_offset = after_journal_offset;
  next.after_journal_sequence = after_journal_sequence;
  next.start_unix_us = start_unix_us;
  next.end_unix_us = end_unix_us;

  wyrebox_daemon_export_job_request_clear (request);
  *request = next;
  next.account_identity = NULL;
  next.dataset_id = NULL;
  next.output_format = NULL;

  return TRUE;
}
