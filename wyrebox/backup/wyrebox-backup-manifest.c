#include "wyrebox-backup-manifest.h"

#include <gio/gio.h>
#include <string.h>

void
wyrebox_backup_manifest_clear (WyreboxBackupManifest *manifest)
{
  if (manifest == NULL)
    return;

  g_clear_pointer (&manifest->backup_id, g_free);
  g_clear_pointer (&manifest->object_store_identity, g_free);
  g_clear_pointer (&manifest->journal_root_dir, g_free);
  g_clear_pointer (&manifest->schema_version, g_free);
  g_clear_pointer (&manifest->rule_package_version, g_free);
  g_clear_pointer (&manifest->view_package_version, g_free);
  memset (manifest, 0, sizeof *manifest);
}

const char *
wyrebox_backup_unit_to_string (WyreboxBackupUnit unit)
{
  switch (unit) {
    case WYREBOX_BACKUP_UNIT_RAW_OBJECTS:
      return "raw-objects";
    case WYREBOX_BACKUP_UNIT_CANONICAL_JOURNAL:
      return "canonical-journal";
    case WYREBOX_BACKUP_UNIT_SCHEMA_METADATA:
      return "schema-metadata";
    case WYREBOX_BACKUP_UNIT_RULE_DEFINITIONS:
      return "rule-definitions";
    case WYREBOX_BACKUP_UNIT_VIEW_DEFINITIONS:
      return "view-definitions";
    case WYREBOX_BACKUP_UNIT_FACT_RECORDS:
      return "fact-records";
    case WYREBOX_BACKUP_UNIT_CONFIGURATION:
      return "configuration";
    case WYREBOX_BACKUP_UNIT_MATERIALIZED_DUCKDB_STATE:
      return "materialized-duckdb-state";
    case WYREBOX_BACKUP_UNIT_SEARCH_INDEXES:
      return "search-indexes";
    case WYREBOX_BACKUP_UNIT_DERIVED_VIEW_CACHE:
      return "derived-view-cache";
    case WYREBOX_BACKUP_UNIT_EXPORT_ARTIFACTS:
      return "export-artifacts";
    case WYREBOX_BACKUP_UNIT_RUNTIME_CACHES:
      return "runtime-caches";
    default:
      return NULL;
  }
}

static gboolean
validate_required_string (const gchar *value, const gchar *field,
    GError **error)
{
  if (value != NULL && value[0] != '\0')
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "backup manifest requires a non-empty %s", field);
  return FALSE;
}

static gboolean
validate_unit_mask (guint32 mask, GError **error)
{
  const guint32 known_units =
      WYREBOX_BACKUP_UNIT_RAW_OBJECTS |
      WYREBOX_BACKUP_UNIT_CANONICAL_JOURNAL |
      WYREBOX_BACKUP_UNIT_SCHEMA_METADATA |
      WYREBOX_BACKUP_UNIT_RULE_DEFINITIONS |
      WYREBOX_BACKUP_UNIT_VIEW_DEFINITIONS |
      WYREBOX_BACKUP_UNIT_FACT_RECORDS |
      WYREBOX_BACKUP_UNIT_CONFIGURATION |
      WYREBOX_BACKUP_UNIT_MATERIALIZED_DUCKDB_STATE |
      WYREBOX_BACKUP_UNIT_SEARCH_INDEXES |
      WYREBOX_BACKUP_UNIT_DERIVED_VIEW_CACHE |
      WYREBOX_BACKUP_UNIT_EXPORT_ARTIFACTS | WYREBOX_BACKUP_UNIT_RUNTIME_CACHES;

  if ((mask & ~known_units) == 0)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA, "backup manifest includes unknown unit bits");
  return FALSE;
}

gboolean
wyrebox_backup_manifest_has_minimal_durable_set (const WyreboxBackupManifest
    *manifest, GError **error)
{
  const guint32 required_units =
      WYREBOX_BACKUP_UNIT_RAW_OBJECTS |
      WYREBOX_BACKUP_UNIT_CANONICAL_JOURNAL |
      WYREBOX_BACKUP_UNIT_SCHEMA_METADATA |
      WYREBOX_BACKUP_UNIT_RULE_DEFINITIONS |
      WYREBOX_BACKUP_UNIT_VIEW_DEFINITIONS |
      WYREBOX_BACKUP_UNIT_FACT_RECORDS | WYREBOX_BACKUP_UNIT_CONFIGURATION;
  guint32 missing_units = 0;

  g_return_val_if_fail (manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  missing_units = required_units & ~manifest->included_units;
  if (missing_units == 0)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "backup manifest is missing required durable units");
  return FALSE;
}

gboolean
wyrebox_backup_manifest_validate (const WyreboxBackupManifest *manifest,
    GError **error)
{
  g_return_val_if_fail (manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_string (manifest->backup_id, "backup ID", error) ||
      !validate_required_string (manifest->object_store_identity,
          "object store identity", error) ||
      !validate_required_string (manifest->journal_root_dir,
          "journal root directory", error) ||
      !validate_required_string (manifest->schema_version,
          "schema version", error) ||
      !validate_required_string (manifest->rule_package_version,
          "rule package version", error) ||
      !validate_required_string (manifest->view_package_version,
          "view package version", error))
    return FALSE;

  if (!validate_unit_mask (manifest->included_units, error))
    return FALSE;

  if (!validate_unit_mask (manifest->rebuildable_units, error))
    return FALSE;

  if ((manifest->included_units &
          (WYREBOX_BACKUP_UNIT_MATERIALIZED_DUCKDB_STATE |
              WYREBOX_BACKUP_UNIT_SEARCH_INDEXES |
              WYREBOX_BACKUP_UNIT_DERIVED_VIEW_CACHE |
              WYREBOX_BACKUP_UNIT_EXPORT_ARTIFACTS |
              WYREBOX_BACKUP_UNIT_RUNTIME_CACHES)) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "backup manifest must not treat rebuildable units as durable");
    return FALSE;
  }

  if (manifest->created_at_unix_us == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "backup manifest requires a non-zero creation timestamp");
    return FALSE;
  }

  if (!wyrebox_backup_manifest_has_minimal_durable_set (manifest, error))
    return FALSE;

  return TRUE;
}
