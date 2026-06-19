#include "wyrebox-journal-event.h"

#include <gio/gio.h>

static void
test_event_record_wraps_journal_record (void)
{
  g_auto (WyreboxJournalEventRecord) event = { 0 };
  g_auto (WyreboxJournalRecord) journal = { 0 };
  g_autoptr (GBytes) payload = NULL;
  g_autoptr (GError) error = NULL;
  const guint8 bytes[] = { 0x01, 0x02, 0x03 };

  payload = g_bytes_new_static (bytes, sizeof bytes);
  journal.offset = 4096;
  journal.sequence = 7;
  journal.event_type = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
  journal.payload = g_bytes_ref (payload);

  g_assert_true (wyrebox_journal_event_record_init_from_reader_record (&event,
          &journal, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (event.journal_offset, ==, 4096);
  g_assert_cmpuint (event.journal_sequence, ==, 7);
  g_assert_cmpstr (wyrebox_journal_event_record_get_event_type_name (&event),
      ==, "MessageDelivered");
  g_assert_true (g_bytes_equal (event.payload, payload));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/journal/event-record/wraps-journal-record",
      test_event_record_wraps_journal_record);

  return g_test_run ();
}
