#pragma once

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum
{
  WYREBOX_BACKUP_UNIT_RAW_OBJECTS = 1u << 0,
  WYREBOX_BACKUP_UNIT_CANONICAL_JOURNAL = 1u << 1,
  WYREBOX_BACKUP_UNIT_SCHEMA_METADATA = 1u << 2,
  WYREBOX_BACKUP_UNIT_RULE_DEFINITIONS = 1u << 3,
  WYREBOX_BACKUP_UNIT_VIEW_DEFINITIONS = 1u << 4,
  WYREBOX_BACKUP_UNIT_FACT_RECORDS = 1u << 5,
  WYREBOX_BACKUP_UNIT_CONFIGURATION = 1u << 6,
  WYREBOX_BACKUP_UNIT_MATERIALIZED_DUCKDB_STATE = 1u << 7,
  WYREBOX_BACKUP_UNIT_SEARCH_INDEXES = 1u << 8,
  WYREBOX_BACKUP_UNIT_DERIVED_VIEW_CACHE = 1u << 9,
  WYREBOX_BACKUP_UNIT_EXPORT_ARTIFACTS = 1u << 10,
  WYREBOX_BACKUP_UNIT_RUNTIME_CACHES = 1u << 11,
} WyreboxBackupUnit;

typedef struct
{
  gchar *backup_id;
  gchar *object_store_identity;
  gchar *journal_root_dir;
  gchar *schema_version;
  gchar *rule_package_version;
  gchar *view_package_version;
  guint64 created_at_unix_us;
  guint32 included_units;
  guint32 rebuildable_units;
} WyreboxBackupManifest;

void wyrebox_backup_manifest_clear (WyreboxBackupManifest *manifest);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxBackupManifest,
    wyrebox_backup_manifest_clear)

gboolean wyrebox_backup_manifest_validate (const WyreboxBackupManifest
    *manifest, GError **error);

gboolean wyrebox_backup_manifest_has_minimal_durable_set (const
    WyreboxBackupManifest *manifest, GError **error);

const char *wyrebox_backup_unit_to_string (WyreboxBackupUnit unit);

G_END_DECLS
/* *INDENT-ON* */
