#include "wyrebox-daemon-export-schema.h"

#include <glib.h>

static void
test_valid_schema_metadata (void)
{
  g_auto (WyreboxDaemonExportSchemaMetadata) metadata = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_export_schema_metadata_init (&metadata,
          "messages.metadata.v1",
          "schema-1",
          "messages.metadata.v1",
          "parquet", "journal-position", "sha256+object-key", &error));
  g_assert_no_error (error);
}

static void
test_missing_schema_metadata_fields_are_rejected (void)
{
  g_auto (WyreboxDaemonExportSchemaMetadata) metadata = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_export_schema_metadata_init (&metadata,
          "",
          "schema-1",
          "messages.metadata.v1",
          "parquet", "journal-position", "sha256+object-key", &error));
  g_assert_nonnull (error);
}

static void
test_export_order_names_are_stable (void)
{
  g_assert_cmpstr (wyrebox_daemon_export_order_to_string
      (WYREBOX_DAEMON_EXPORT_ORDER_BY_TIME_RANGE), ==, "time-range");
  g_assert_null (wyrebox_daemon_export_order_to_string (
          (WyreboxDaemonExportOrder) 0));
}

static void
test_object_reference_contract_is_stable (void)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_export_object_reference_contract_is_stable
      ("sha256+object-key", &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_export_object_reference_contract_is_stable
      ("sha512+object-key", &error));
  g_assert_nonnull (error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/export-schema/valid",
      test_valid_schema_metadata);
  g_test_add_func ("/daemon-api/export-schema/missing-fields",
      test_missing_schema_metadata_fields_are_rejected);
  g_test_add_func ("/daemon-api/export-schema/order-names",
      test_export_order_names_are_stable);
  g_test_add_func ("/daemon-api/export-schema/object-reference-contract",
      test_object_reference_contract_is_stable);

  return g_test_run ();
}
