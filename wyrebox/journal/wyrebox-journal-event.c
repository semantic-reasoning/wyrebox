#include "wyrebox-journal-event.h"

#include <gio/gio.h>

static const WyreboxJournalEventTypeDescriptor catalog[] = {
  {
        WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
        "MessageDelivered",
        "journal.payload.message-delivered.v1",
        "Durable inbound message delivery",
      },
  {
        WYREBOX_JOURNAL_EVENT_FLAG_CHANGED,
        "FlagChanged",
        "journal.payload.flag-changed.v1",
        "Flag mutation event",
      },
  {
        WYREBOX_JOURNAL_EVENT_KEYWORD_CHANGED,
        "KeywordChanged",
        "journal.payload.keyword-changed.v1",
        "Keyword mutation event",
      },
  {
        WYREBOX_JOURNAL_EVENT_FACT_INSERTED,
        "FactInserted",
        "journal.payload.fact-inserted.v1",
        "Fact insertion event",
      },
  {
        WYREBOX_JOURNAL_EVENT_FACT_RETRACTED,
        "FactRetracted",
        "journal.payload.fact-retracted.v1",
        "Fact retraction event",
      },
  {
        WYREBOX_JOURNAL_EVENT_DERIVED_VIEW_MEMBERSHIP_CHANGED,
        "DerivedViewMembershipChanged",
        "journal.payload.derived-view-membership-changed.v1",
        "Derived view membership event",
      },
  {
        WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED,
        "DaemonAuditRecorded",
        "journal.payload.daemon-audit-recorded.v1",
        "Administrative repair or policy action",
      },
};

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

const WyreboxJournalEventTypeDescriptor *
wyrebox_journal_event_type_catalog_lookup (WyreboxJournalEventType event_type)
{
  for (gsize index = 0; index < G_N_ELEMENTS (catalog); index++) {
    if (catalog[index].event_type == event_type)
      return &catalog[index];
  }

  return NULL;
}

gsize
wyrebox_journal_event_type_catalog_size (void)
{
  return G_N_ELEMENTS (catalog);
}

const WyreboxJournalEventTypeDescriptor *
wyrebox_journal_event_type_catalog_at (gsize index)
{
  if (index >= G_N_ELEMENTS (catalog))
    return NULL;

  return &catalog[index];
}
