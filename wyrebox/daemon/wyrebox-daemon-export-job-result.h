#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum
{
  WYREBOX_DAEMON_EXPORT_JOB_STATUS_QUEUED,
  WYREBOX_DAEMON_EXPORT_JOB_STATUS_RUNNING,
  WYREBOX_DAEMON_EXPORT_JOB_STATUS_SUCCEEDED,
  WYREBOX_DAEMON_EXPORT_JOB_STATUS_FAILED,
} WyreboxDaemonExportJobStatus;

typedef struct
{
  char *job_id;
  char *account_identity;
  char *dataset_id;
  WyreboxDaemonExportJobStatus status;
  guint64 created_at_unix_us;
  guint64 started_at_unix_us;
  guint64 completed_at_unix_us;
  char *result_object_path;
} WyreboxDaemonExportJobResult;

void wyrebox_daemon_export_job_result_clear (
    WyreboxDaemonExportJobResult *result);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonExportJobResult,
    wyrebox_daemon_export_job_result_clear)

gboolean wyrebox_daemon_export_job_result_init (
    WyreboxDaemonExportJobResult *result,
    const char *job_id,
    const char *account_identity,
    const char *dataset_id,
    WyreboxDaemonExportJobStatus status,
    guint64 created_at_unix_us,
    guint64 started_at_unix_us,
    guint64 completed_at_unix_us,
    const char *result_object_path,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
