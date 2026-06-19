#include "wyrebox-daemon-export-dataset-catalog.h"

#include <gio/gio.h>

static void
test_catalog_resolves_message_metadata (void)
{
  const WyreboxDaemonExportDatasetDescriptor *descriptor = NULL;

  descriptor =
      wyrebox_daemon_export_dataset_catalog_lookup ("messages.metadata.v1");
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->name, ==, "message metadata");
  g_assert_cmpstr (descriptor->schema_version, ==, "schema.metadata.v1");
  g_assert_cmpstr (descriptor->output_format, ==, "parquet");
}

static void
test_catalog_resolves_object_storage_statistics (void)
{
  const WyreboxDaemonExportDatasetDescriptor *descriptor = NULL;

  descriptor =
      wyrebox_daemon_export_dataset_catalog_lookup
      ("object-storage.statistics.v1");
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->name, ==, "object storage statistics");
  g_assert_cmpstr (descriptor->schema_version, ==,
      "schema.object-storage-statistics.v1");
}

static void
test_catalog_rejects_unknown_dataset (void)
{
  g_assert_null (wyrebox_daemon_export_dataset_catalog_lookup
      ("unknown.dataset.v1"));
  g_assert_null (wyrebox_daemon_export_dataset_catalog_lookup (""));
  g_assert_null (wyrebox_daemon_export_dataset_catalog_lookup (NULL));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/daemon-api/export-dataset-catalog/resolves-message-metadata",
      test_catalog_resolves_message_metadata);
  g_test_add_func
      ("/daemon-api/export-dataset-catalog/resolves-object-storage-statistics",
      test_catalog_resolves_object_storage_statistics);
  g_test_add_func ("/daemon-api/export-dataset-catalog/rejects-unknown-dataset",
      test_catalog_rejects_unknown_dataset);

  return g_test_run ();
}
