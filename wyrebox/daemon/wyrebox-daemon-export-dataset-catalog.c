#include "wyrebox-daemon-export-dataset-catalog.h"

#include <gio/gio.h>

static const WyreboxDaemonExportDatasetDescriptor catalog[] = {
  {
        "messages.metadata.v1",
        "message metadata",
        "schema.metadata.v1",
        "parquet",
        "Stable message-level analytical export with header and object "
        "references",
      },
  {
        "mailbox.memberships.v1",
        "mailbox memberships",
        "schema.membership.v1",
        "parquet",
        "Stable mailbox membership export with UID and visibility state",
      },
  {
        "events.stream.v1",
        "event stream records",
        "schema.event-stream.v1",
        "parquet",
        "Append-only canonical event export ordered by journal position",
      },
  {
        "facts.records.v1",
        "fact records",
        "schema.fact-record.v1",
        "parquet",
        "Canonical fact export with source and provenance metadata",
      },
  {
        "derived-views.memberships.v1",
        "derived view memberships",
        "schema.derived-view-membership.v1",
        "parquet",
        "Derived mailbox membership export for virtual views",
      },
  {
        "object-storage.statistics.v1",
        "object storage statistics",
        "schema.object-storage-statistics.v1",
        "parquet",
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
