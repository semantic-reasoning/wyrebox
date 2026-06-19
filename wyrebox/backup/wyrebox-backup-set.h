#pragma once

#include "wyrebox-backup-manifest.h"

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  gchar *unit_name;
  gchar *relative_path;
  gchar *digest;
  guint64 size_bytes;
} WyreboxBackupSetEntry;

typedef struct
{
  gchar *backup_id;
  GPtrArray *entries;
} WyreboxBackupSet;

void wyrebox_backup_set_entry_clear (WyreboxBackupSetEntry *entry);
void wyrebox_backup_set_entry_free (WyreboxBackupSetEntry *entry);
void wyrebox_backup_set_clear (WyreboxBackupSet *set);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxBackupSetEntry,
    wyrebox_backup_set_entry_clear)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxBackupSet,
    wyrebox_backup_set_clear)

gboolean wyrebox_backup_set_validate_against_manifest (const WyreboxBackupSet
    *set, const WyreboxBackupManifest *manifest, GError **error);

gboolean wyrebox_backup_set_has_complete_durable_units (const WyreboxBackupSet
    *set, const WyreboxBackupManifest *manifest, GError **error);

G_END_DECLS
/* *INDENT-ON* */
