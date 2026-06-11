#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define WYREBOX_TYPE_JOURNAL_WRITER (wyrebox_journal_writer_get_type())

G_DECLARE_FINAL_TYPE(
  WyreboxJournalWriter,
  wyrebox_journal_writer,
  WYREBOX,
  JOURNAL_WRITER,
  GObject)

typedef enum {
  WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
  WYREBOX_JOURNAL_EVENT_FLAG_CHANGED,
  WYREBOX_JOURNAL_EVENT_KEYWORD_CHANGED,
  WYREBOX_JOURNAL_EVENT_FACT_INSERTED,
  WYREBOX_JOURNAL_EVENT_FACT_RETRACTED,
  WYREBOX_JOURNAL_EVENT_DERIVED_VIEW_MEMBERSHIP_CHANGED,
} WyreboxJournalEventType;

const char *wyrebox_journal_event_type_to_string(WyreboxJournalEventType event_type);

WyreboxJournalWriter *wyrebox_journal_writer_new(const char *journal_root_dir,
                                                 GError **error);

gboolean wyrebox_journal_writer_append(WyreboxJournalWriter *self,
                                       WyreboxJournalEventType event_type,
                                       GBytes *payload,
                                       guint64 *out_offset,
                                       guint64 *out_sequence,
                                       GError **error);

G_END_DECLS
