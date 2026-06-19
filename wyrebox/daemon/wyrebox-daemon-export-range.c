#include "wyrebox-daemon-export-range.h"

#include <gio/gio.h>

static gboolean
validate_required_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "export range %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "export range %s must not contain control characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_cursor_token (const char *value, GError **error)
{
  if (value == NULL)
    return TRUE;

  if (value[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "export range cursor_token must not be empty when present");
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_journal_cursor (guint64 after_journal_offset,
    guint64 after_journal_sequence, GError **error)
{
  if (after_journal_offset != 0 && after_journal_sequence == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "export range requires a non-zero afterJournalSequence when "
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
        "export range start_unix_us must not be greater than end_unix_us");
    return FALSE;
  }

  return TRUE;
}

void
wyrebox_daemon_export_range_clear (WyreboxDaemonExportRange *range)
{
  if (range == NULL)
    return;

  g_clear_pointer (&range->account_identity, g_free);
  g_clear_pointer (&range->dataset_id, g_free);
  g_clear_pointer (&range->cursor_token, g_free);
  range->after_journal_offset = 0;
  range->after_journal_sequence = 0;
  range->start_unix_us = 0;
  range->end_unix_us = 0;
  range->range_kind = 0;
}

const char *
wyrebox_daemon_export_range_kind_to_string (WyreboxDaemonExportRangeKind kind)
{
  switch (kind) {
    case WYREBOX_DAEMON_EXPORT_RANGE_KIND_JOURNAL_OFFSET:
      return "journal-offset";
    case WYREBOX_DAEMON_EXPORT_RANGE_KIND_TIME_RANGE:
      return "time-range";
    default:
      return NULL;
  }
}

gboolean
wyrebox_daemon_export_range_init (WyreboxDaemonExportRange *range,
    const char *account_identity,
    const char *dataset_id,
    const char *cursor_token,
    WyreboxDaemonExportRangeKind range_kind,
    guint64 after_journal_offset,
    guint64 after_journal_sequence,
    guint64 start_unix_us, guint64 end_unix_us, GError **error)
{
  g_auto (WyreboxDaemonExportRange) next = { 0 };

  g_return_val_if_fail (range != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (wyrebox_daemon_export_range_kind_to_string (range_kind) == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "export range kind is invalid");
    return FALSE;
  }

  if (!validate_required_text (account_identity, "account_identity", error) ||
      !validate_required_text (dataset_id, "dataset_id", error) ||
      !validate_cursor_token (cursor_token, error) ||
      !validate_journal_cursor (after_journal_offset, after_journal_sequence,
          error) || !validate_time_range (start_unix_us, end_unix_us, error))
    return FALSE;

  if (range_kind == WYREBOX_DAEMON_EXPORT_RANGE_KIND_JOURNAL_OFFSET &&
      (start_unix_us != 0 || end_unix_us != 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "journal-offset export range must not carry a time range");
    return FALSE;
  }

  if (range_kind == WYREBOX_DAEMON_EXPORT_RANGE_KIND_TIME_RANGE &&
      (after_journal_offset != 0 || after_journal_sequence != 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "time-range export range must not carry a journal cursor");
    return FALSE;
  }

  next.account_identity = g_strdup (account_identity);
  next.dataset_id = g_strdup (dataset_id);
  next.cursor_token = g_strdup (cursor_token);
  next.after_journal_offset = after_journal_offset;
  next.after_journal_sequence = after_journal_sequence;
  next.start_unix_us = start_unix_us;
  next.end_unix_us = end_unix_us;
  next.range_kind = range_kind;

  wyrebox_daemon_export_range_clear (range);
  *range = next;
  next.account_identity = NULL;
  next.dataset_id = NULL;
  next.cursor_token = NULL;

  return TRUE;
}
