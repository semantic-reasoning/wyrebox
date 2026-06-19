#include "wyrebox-journal-event.h"

#include <gio/gio.h>

void
wyrebox_journal_event_record_clear (WyreboxJournalEventRecord *record)
{
  if (record == NULL)
    return;

  g_clear_pointer (&record->payload, g_bytes_unref);
  record->journal_offset = 0;
  record->journal_sequence = 0;
  record->event_type = 0;
}

gboolean
wyrebox_journal_event_record_init_from_reader_record (WyreboxJournalEventRecord
    *record, const WyreboxJournalRecord *journal, GError **error)
{
  g_auto (WyreboxJournalEventRecord) next = { 0 };

  g_return_val_if_fail (record != NULL, FALSE);
  g_return_val_if_fail (journal != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (journal->payload == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "journal event record payload is required");
    return FALSE;
  }

  next.journal_offset = journal->offset;
  next.journal_sequence = journal->sequence;
  next.event_type = journal->event_type;
  next.payload = g_bytes_ref (journal->payload);

  wyrebox_journal_event_record_clear (record);
  *record = next;
  next.payload = NULL;
  return TRUE;
}

const char *
wyrebox_journal_event_record_get_event_type_name (const
    WyreboxJournalEventRecord *record)
{
  g_return_val_if_fail (record != NULL, NULL);

  return wyrebox_journal_event_type_to_string (record->event_type);
}
