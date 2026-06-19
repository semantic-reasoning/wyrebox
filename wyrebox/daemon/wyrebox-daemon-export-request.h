#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  char *account_identity;
  char *dataset_id;
  char *output_format;
  guint64 after_journal_offset;
  guint64 after_journal_sequence;
  guint64 start_unix_us;
  guint64 end_unix_us;
} WyreboxDaemonExportRequest;

void wyrebox_daemon_export_request_clear (
    WyreboxDaemonExportRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonExportRequest,
    wyrebox_daemon_export_request_clear)

gboolean wyrebox_daemon_export_request_init (
    WyreboxDaemonExportRequest *request,
    const char *account_identity,
    const char *dataset_id,
    const char *output_format,
    guint64 after_journal_offset,
    guint64 after_journal_sequence,
    guint64 start_unix_us,
    guint64 end_unix_us,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
