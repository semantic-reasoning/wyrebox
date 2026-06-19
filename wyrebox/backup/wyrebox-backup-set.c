#include "wyrebox-backup-set.h"

#include <gio/gio.h>
#include <string.h>

static gboolean
validate_required_string (const gchar *value, const gchar *field,
    GError **error)
{
  if (value != NULL && value[0] != '\0')
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA, "backup set requires a non-empty %s", field);
  return FALSE;
}

void
wyrebox_backup_set_entry_clear (WyreboxBackupSetEntry *entry)
{
  if (entry == NULL)
    return;

  g_clear_pointer (&entry->unit_name, g_free);
  g_clear_pointer (&entry->relative_path, g_free);
  g_clear_pointer (&entry->digest, g_free);
  memset (entry, 0, sizeof *entry);
}

void
wyrebox_backup_set_clear (WyreboxBackupSet *set)
{
  if (set == NULL)
    return;

  g_clear_pointer (&set->backup_id, g_free);
  if (set->entries != NULL) {
    for (guint i = 0; i < set->entries->len; i++) {
      WyreboxBackupSetEntry *entry = g_ptr_array_index (set->entries, i);

      wyrebox_backup_set_entry_clear (entry);
      g_free (entry);
    }
    g_ptr_array_free (set->entries, TRUE);
  }

  memset (set, 0, sizeof *set);
}

static gboolean
validate_entry (const WyreboxBackupSetEntry *entry, GHashTable *seen_units,
    GError **error)
{
  g_autofree char *key = NULL;

  if (!validate_required_string (entry->unit_name, "unit name", error) ||
      !validate_required_string (entry->relative_path, "relative path",
          error) || !validate_required_string (entry->digest, "digest", error))
    return FALSE;

  if (entry->size_bytes == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "backup set entry %s has zero size", entry->relative_path);
    return FALSE;
  }

  key = g_strdup (entry->unit_name);
  if (g_hash_table_contains (seen_units, key)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "backup set contains duplicate unit %s", entry->unit_name);
    return FALSE;
  }

  g_hash_table_add (seen_units, g_steal_pointer (&key));
  return TRUE;
}

gboolean
wyrebox_backup_set_validate_against_manifest (const WyreboxBackupSet *set,
    const WyreboxBackupManifest *manifest, GError **error)
{
  GHashTable *seen_units = NULL;
  guint32 required_units = 0;
  guint32 present_units = 0;

  g_return_val_if_fail (set != NULL, FALSE);
  g_return_val_if_fail (manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_string (set->backup_id, "backup ID", error))
    return FALSE;

  if (g_strcmp0 (set->backup_id, manifest->backup_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "backup set and manifest backup IDs do not match");
    return FALSE;
  }

  seen_units = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < set->entries->len; i++) {
    WyreboxBackupSetEntry *entry = g_ptr_array_index (set->entries, i);
    const WyreboxBackupUnit unit = 1u << i;

    if (!validate_entry (entry, seen_units, error)) {
      g_hash_table_destroy (seen_units);
      return FALSE;
    }

    if (wyrebox_backup_unit_to_string (unit) == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "backup set entry %s refers to an unknown unit", entry->unit_name);
      g_hash_table_destroy (seen_units);
      return FALSE;
    }

    present_units |= unit;
  }

  required_units = manifest->included_units;
  if ((required_units & ~present_units) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "backup set is missing required durable units");
    g_hash_table_destroy (seen_units);
    return FALSE;
  }

  g_hash_table_destroy (seen_units);
  return TRUE;
}

gboolean
wyrebox_backup_set_has_complete_durable_units (const WyreboxBackupSet *set,
    const WyreboxBackupManifest *manifest, GError **error)
{
  g_return_val_if_fail (set != NULL, FALSE);
  g_return_val_if_fail (manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_backup_manifest_has_minimal_durable_set (manifest, error))
    return FALSE;

  return wyrebox_backup_set_validate_against_manifest (set, manifest, error);
}
