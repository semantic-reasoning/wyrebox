#include "wyrebox-daemon-export-job-result.h"

#include <gio/gio.h>

static gboolean
validate_required_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "export job result %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "export job result %s must not contain control characters",
          field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_status (WyreboxDaemonExportJobStatus status, GError **error)
{
  switch (status) {
    case WYREBOX_DAEMON_EXPORT_JOB_STATUS_QUEUED:
    case WYREBOX_DAEMON_EXPORT_JOB_STATUS_RUNNING:
    case WYREBOX_DAEMON_EXPORT_JOB_STATUS_SUCCEEDED:
    case WYREBOX_DAEMON_EXPORT_JOB_STATUS_FAILED:
      return TRUE;
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT, "export job result status is invalid");
      return FALSE;
  }
}

void
wyrebox_daemon_export_job_result_clear (WyreboxDaemonExportJobResult *result)
{
  if (result == NULL)
    return;

  g_clear_pointer (&result->job_id, g_free);
  g_clear_pointer (&result->account_identity, g_free);
  g_clear_pointer (&result->dataset_id, g_free);
  g_clear_pointer (&result->result_object_path, g_free);
  result->status = WYREBOX_DAEMON_EXPORT_JOB_STATUS_QUEUED;
  result->created_at_unix_us = 0;
  result->started_at_unix_us = 0;
  result->completed_at_unix_us = 0;
}

gboolean
wyrebox_daemon_export_job_result_init (WyreboxDaemonExportJobResult *result,
    const char *job_id, const char *account_identity, const char *dataset_id,
    WyreboxDaemonExportJobStatus status, guint64 created_at_unix_us,
    guint64 started_at_unix_us, guint64 completed_at_unix_us,
    const char *result_object_path, GError **error)
{
  g_auto (WyreboxDaemonExportJobResult) next = { 0 };

  g_return_val_if_fail (result != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_text (job_id, "job_id", error) ||
      !validate_required_text (account_identity, "account_identity", error) ||
      !validate_required_text (dataset_id, "dataset_id", error) ||
      !validate_status (status, error))
    return FALSE;

  if (started_at_unix_us != 0 && created_at_unix_us != 0 &&
      started_at_unix_us < created_at_unix_us) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "export job result started_at_unix_us must not precede created_at_unix_us");
    return FALSE;
  }

  if (completed_at_unix_us != 0 && started_at_unix_us != 0 &&
      completed_at_unix_us < started_at_unix_us) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "export job result completed_at_unix_us must not precede started_at_unix_us");
    return FALSE;
  }

  next.job_id = g_strdup (job_id);
  next.account_identity = g_strdup (account_identity);
  next.dataset_id = g_strdup (dataset_id);
  next.status = status;
  next.created_at_unix_us = created_at_unix_us;
  next.started_at_unix_us = started_at_unix_us;
  next.completed_at_unix_us = completed_at_unix_us;
  next.result_object_path = g_strdup (result_object_path);

  wyrebox_daemon_export_job_result_clear (result);
  *result = next;
  next.job_id = NULL;
  next.account_identity = NULL;
  next.dataset_id = NULL;
  next.result_object_path = NULL;

  return TRUE;
}
