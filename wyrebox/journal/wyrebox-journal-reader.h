#pragma once

#include <glib-object.h>

#include "wyrebox-journal-writer.h"

/* *INDENT-OFF* */
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

typedef enum {
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_EOF,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_MISSING_SEGMENT,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_EMPTY_SEGMENT,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_PARTIAL_HEADER,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_PARTIAL_RECORD,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_MAGIC,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_HEADER_SIZE,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_VERSION,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SEQUENCE,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SIZE,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_ZERO_EVENT_TYPE_LENGTH,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_UNKNOWN_EVENT_TYPE,
  WYREBOX_JOURNAL_SAFE_PREFIX_STOP_CHECKSUM_MISMATCH,
} WyreboxJournalSafePrefixStopReason;

typedef struct {
  guint64 safe_end_offset;
  guint64 last_safe_sequence;
  gboolean has_last_safe_sequence;
  gboolean reached_eof;
  gboolean unsafe_suffix_found;
  WyreboxJournalSafePrefixStopReason stop_reason;
  guint64 unsafe_offset;
  guint64 unsafe_available_size;
  guint64 unsafe_required_size;
} WyreboxJournalSafePrefix;

void wyrebox_journal_record_clear (WyreboxJournalRecord *record);

WyreboxJournalReader *wyrebox_journal_reader_new (const char *journal_root_dir,
    GError **error);

gboolean wyrebox_journal_reader_read_next (WyreboxJournalReader *self,
    WyreboxJournalRecord *record, gboolean *out_eof, GError **error);

gboolean wyrebox_journal_reader_scan_safe_prefix (WyreboxJournalReader *self,
    WyreboxJournalSafePrefix *out_prefix, GError **error);

/*
 * Scans an already-open journal segment fd.
 *
 * The scan duplicates @segment_fd and owns the duplicate; the caller retains
 * ownership of @segment_fd. The readable size is captured with fstat() before
 * scanning.
 */
gboolean wyrebox_journal_reader_scan_safe_prefix_for_segment_fd (int
    segment_fd, const char *segment_description,
    WyreboxJournalSafePrefix *out_prefix, GError **error);

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
/* *INDENT-ON* */
