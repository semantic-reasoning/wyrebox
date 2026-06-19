#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum
{
  WYREBOX_DAEMON_EXPORT_RANGE_KIND_JOURNAL_OFFSET = 1,
  WYREBOX_DAEMON_EXPORT_RANGE_KIND_TIME_RANGE = 2,
} WyreboxDaemonExportRangeKind;

typedef struct
{
  char *account_identity;
  char *dataset_id;
  char *cursor_token;
  guint64 after_journal_offset;
  guint64 after_journal_sequence;
  guint64 start_unix_us;
  guint64 end_unix_us;
  WyreboxDaemonExportRangeKind range_kind;
} WyreboxDaemonExportRange;

void wyrebox_daemon_export_range_clear (WyreboxDaemonExportRange *range);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonExportRange,
    wyrebox_daemon_export_range_clear)

gboolean wyrebox_daemon_export_range_init (WyreboxDaemonExportRange *range,
    const char *account_identity,
    const char *dataset_id,
    const char *cursor_token,
    WyreboxDaemonExportRangeKind range_kind,
    guint64 after_journal_offset,
    guint64 after_journal_sequence,
    guint64 start_unix_us,
    guint64 end_unix_us,
    GError **error);

const char *wyrebox_daemon_export_range_kind_to_string (
    WyreboxDaemonExportRangeKind kind);

G_END_DECLS
/* *INDENT-ON* */
