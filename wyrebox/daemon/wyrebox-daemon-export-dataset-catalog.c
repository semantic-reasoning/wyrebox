#include "wyrebox-daemon-export-dataset-catalog.h"

#include <gio/gio.h>

static const WyreboxDaemonExportDatasetDescriptor catalog[] = {
  {
        "messages.metadata.v1",
        "message metadata",
        "schema.metadata.v1",
        "parquet",
        "journal-offset",
        "journal_offset ASC, journal_sequence ASC, message_id ASC",
        "account_identity",
        "Stable message-level analytical export with header and object "
        "references",
      },
  {
        "mailbox.memberships.v1",
        "mailbox memberships",
        "schema.membership.v1",
        "parquet",
        "journal-offset",
        "journal_offset ASC, journal_sequence ASC, mailbox_id ASC",
        "account_identity",
        "Stable mailbox membership export with UID and visibility state",
      },
  {
        "events.stream.v1",
        "event stream records",
        "schema.event-stream.v1",
        "parquet",
        "journal-offset",
        "journal_offset ASC, journal_sequence ASC, event_id ASC",
        "account_identity",
        "Append-only canonical event export ordered by journal position",
      },
  {
        "facts.records.v1",
        "fact records",
        "schema.fact-record.v1",
        "parquet",
        "journal-offset",
        "journal_offset ASC, journal_sequence ASC, fact_id ASC",
        "account_identity",
        "Canonical fact export with source and provenance metadata",
      },
  {
        "derived-views.memberships.v1",
        "derived view memberships",
        "schema.derived-view-membership.v1",
        "parquet",
        "journal-offset",
        "journal_offset ASC, journal_sequence ASC, derived_view_id ASC",
        "account_identity",
        "Derived mailbox membership export for virtual views",
      },
  {
        "object-storage.statistics.v1",
        "object storage statistics",
        "schema.object-storage-statistics.v1",
        "parquet",
        "time-range",
        "captured_at_unix_us ASC, bucket_name ASC",
        "account_identity",
        "Object storage inventory and size statistics",
      },
};

const WyreboxDaemonExportDatasetDescriptor *
wyrebox_daemon_export_dataset_catalog_lookup (const char *dataset_id)
{
  if (dataset_id == NULL || *dataset_id == '\0')
    return NULL;

  for (gsize i = 0; i < G_N_ELEMENTS (catalog); i++) {
    if (g_strcmp0 (catalog[i].dataset_id, dataset_id) == 0)
      return &catalog[i];
  }

  return NULL;
}

gsize
wyrebox_daemon_export_dataset_catalog_size (void)
{
  return G_N_ELEMENTS (catalog);
}

const WyreboxDaemonExportDatasetDescriptor *
wyrebox_daemon_export_dataset_catalog_at (gsize index)
{
  if (index >= G_N_ELEMENTS (catalog))
    return NULL;

  return &catalog[index];
}
