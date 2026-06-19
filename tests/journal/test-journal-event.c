#include "wyrebox-journal-event.h"

#include <gio/gio.h>

static void
test_event_type_catalog_is_enumerable (void)
{
  g_assert_cmpuint (wyrebox_journal_event_type_catalog_size (), ==, 7);
  g_assert_nonnull (wyrebox_journal_event_type_catalog_at (0));
  g_assert_nonnull (wyrebox_journal_event_type_catalog_at (6));
  g_assert_null (wyrebox_journal_event_type_catalog_at (7));
}

static void
test_event_type_catalog_resolves_known_event_types (void)
{
  const WyreboxJournalEventTypeDescriptor *descriptor = NULL;

  descriptor = wyrebox_journal_event_type_catalog_lookup
      (WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED);
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->event_type_name, ==, "MessageDelivered");
  g_assert_cmpstr (descriptor->payload_schema_version, ==,
      "journal.payload.message-delivered.v1");

  descriptor = wyrebox_journal_event_type_catalog_lookup
      (WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED);
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->event_type_name, ==, "DaemonAuditRecorded");
}

static void
test_event_type_catalog_preserves_string_names (void)
{
  const WyreboxJournalEventTypeDescriptor *descriptor = NULL;

  descriptor = wyrebox_journal_event_type_catalog_at (2);
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->event_type_name, ==, "KeywordChanged");
  g_assert_cmpstr (descriptor->description, ==, "Keyword mutation event");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/journal/event-type-catalog/is-enumerable",
      test_event_type_catalog_is_enumerable);
  g_test_add_func ("/journal/event-type-catalog/resolves-known-event-types",
      test_event_type_catalog_resolves_known_event_types);
  g_test_add_func ("/journal/event-type-catalog/preserves-string-names",
      test_event_type_catalog_preserves_string_names);

  return g_test_run ();
}
