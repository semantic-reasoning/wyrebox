#pragma once

#include <glib-object.h>

#include "wyrebox-journal-writer.h"

G_BEGIN_DECLS
#define WYREBOX_TYPE_JOURNAL_READER (wyrebox_journal_reader_get_type())
G_DECLARE_FINAL_TYPE (WyreboxJournalReader,
    wyrebox_journal_reader, WYREBOX, JOURNAL_READER, GObject)
     typedef struct
     {
       guint64 offset;
       guint64 sequence;
       WyreboxJournalEventType event_type;
       GBytes *payload;         /* transfer full when copied out */
     } WyreboxJournalRecord;

     void wyrebox_journal_record_clear (WyreboxJournalRecord *record);

     WyreboxJournalReader *wyrebox_journal_reader_new (const char
    *journal_root_dir, GError **error);

     gboolean wyrebox_journal_reader_read_next (WyreboxJournalReader *self,
    WyreboxJournalRecord *record, gboolean *out_eof, GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxJournalRecord,
    wyrebox_journal_record_clear)
    G_END_DECLS
