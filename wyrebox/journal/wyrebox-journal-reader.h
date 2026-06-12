#pragma once

#include <glib-object.h>

#include "wyrebox-journal-writer.h"

G_BEGIN_DECLS

#define WYREBOX_TYPE_JOURNAL_READER (wyrebox_journal_reader_get_type())

G_DECLARE_FINAL_TYPE (WyreboxJournalReader,
    wyrebox_journal_reader,
    WYREBOX,
    JOURNAL_READER,
    GObject)

typedef struct {
  guint64 offset;
  guint64 sequence;
  WyreboxJournalEventType event_type;
  /*
   * Owned by the record. Call wyrebox_journal_record_clear() to release it,
   * or take a reference with g_bytes_ref() before retaining it elsewhere.
   */
  GBytes *payload;
} WyreboxJournalRecord;

void wyrebox_journal_record_clear (WyreboxJournalRecord *record);

WyreboxJournalReader *wyrebox_journal_reader_new (const char *journal_root_dir,
    GError **error);

gboolean wyrebox_journal_reader_read_next (WyreboxJournalReader *self,
    WyreboxJournalRecord *record, gboolean *out_eof, GError **error);

/*
 * Validates and consumes the journal record at @checkpoint_offset.
 *
 * On success, the record at @checkpoint_offset must have
 * @checkpoint_sequence and pass the same validation as read_next(); the next
 * read returns the following record or EOF. On failure, the reader position and
 * expected sequence are unspecified; discard and recreate the reader before
 * retrying or continuing.
 */
gboolean wyrebox_journal_reader_seek_after_checkpoint (WyreboxJournalReader
    *self, guint64 checkpoint_offset, guint64 checkpoint_sequence,
    GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxJournalRecord,
    wyrebox_journal_record_clear)

G_END_DECLS
