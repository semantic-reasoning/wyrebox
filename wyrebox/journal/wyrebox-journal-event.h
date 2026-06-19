#pragma once

#include "wyrebox-journal-reader.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  guint64 journal_offset;
  guint64 journal_sequence;
  WyreboxJournalEventType event_type;
  GBytes *payload;
} WyreboxJournalEventRecord;

void wyrebox_journal_event_record_clear (WyreboxJournalEventRecord *record);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxJournalEventRecord,
    wyrebox_journal_event_record_clear)

gboolean wyrebox_journal_event_record_init_from_reader_record (
    WyreboxJournalEventRecord *record, const WyreboxJournalRecord *journal,
    GError **error);

const char *wyrebox_journal_event_record_get_event_type_name (
    const WyreboxJournalEventRecord *record);

G_END_DECLS
/* *INDENT-ON* */
