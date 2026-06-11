/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-migration.h"

#include <glib.h>
#include <string.h>

struct _WyreboxSchemaMigration
{
  GObject parent_instance;
};

G_DEFINE_TYPE (WyreboxSchemaMigration, wyrebox_schema_migration, G_TYPE_OBJECT);

/*
 * Keep these small and explicit for fixture-backed deterministic transitions.
 */
#define WYREBOX_SCHEMA_VERSION_FIRST 1
#define WYREBOX_SCHEMA_VERSION_CURRENT 1
#define WYREBOX_SCHEMA_VERSION_LEGACY_0 0

void wyrebox_schema_migration_metadata_state_clear
    (WyreboxSchemaMigrationMetadataState * state)
{
  if (state == NULL)
    return;

  memset (state, 0, sizeof *state);
}

guint64
wyrebox_schema_migration_get_first_supported_schema_version (void)
{
  return (guint64) WYREBOX_SCHEMA_VERSION_FIRST;
}

guint64
wyrebox_schema_migration_get_current_schema_version (void)
{
  return (guint64) WYREBOX_SCHEMA_VERSION_CURRENT;
}

static void
wyrebox_schema_migration_class_init (WyreboxSchemaMigrationClass *klass)
{
}

static void
wyrebox_schema_migration_init (WyreboxSchemaMigration *self)
{
}

WyreboxSchemaMigration *
wyrebox_schema_migration_new (void)
{
  return g_object_new (WYREBOX_TYPE_SCHEMA_MIGRATION, NULL);
}

static gboolean
metadata_version_is_supported_plan_target (guint64 target_version,
    GError **error)
{
  guint64 current_version =
      wyrebox_schema_migration_get_current_schema_version ();

  if (target_version > current_version) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        "target schema version %" G_GUINT64_FORMAT
        " is above current supported version %" G_GUINT64_FORMAT,
        target_version, current_version);
    return FALSE;
  }

  return TRUE;
}

static gboolean
metadata_version_to_target (guint64 metadata_version, guint64 target_version,
    GError **error)
{
  guint64 current_version =
      wyrebox_schema_migration_get_current_schema_version ();

  if (metadata_version > current_version) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        "schema version %" G_GUINT64_FORMAT
        " is newer than current supported version %" G_GUINT64_FORMAT,
        metadata_version, current_version);
    return FALSE;
  }

  if (target_version < metadata_version) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "target schema version %" G_GUINT64_FORMAT
        " would downgrade from %s%" G_GUINT64_FORMAT,
        target_version,
        metadata_version == 0 ? "legacy " : "", metadata_version);
    return FALSE;
  }

  if (metadata_version == current_version)
    return TRUE;

  if (metadata_version == WYREBOX_SCHEMA_VERSION_LEGACY_0 &&
      target_version == current_version) {
    return TRUE;
  }

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "no migration plan from schema version %" G_GUINT64_FORMAT
      " to %" G_GUINT64_FORMAT, metadata_version, target_version);
  return FALSE;
}

static gboolean
apply_metadata_plan (guint64 metadata_version, guint64 target_version,
    WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  if (!metadata_version_is_supported_plan_target (target_version, error))
    return FALSE;

  if (!metadata_version_to_target (metadata_version, target_version, error))
    return FALSE;

  state->schema_version_present = TRUE;
  state->schema_version = target_version;

  return TRUE;
}

gboolean
wyrebox_schema_migration_evaluate_to_version (WyreboxSchemaMigration *self,
    WyreboxSchemaMigrationMetadataState *state,
    guint64 target_version, GError **error)
{
  guint64 metadata_version = 0;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_MIGRATION (self), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (state->schema_version_present)
    metadata_version = state->schema_version;
  else
    metadata_version = wyrebox_schema_migration_get_current_schema_version ();

  /*
   * NOTE: apply_metadata_plan mutates @state only after validation succeeds.
   */
  if (!apply_metadata_plan (metadata_version, target_version, state, error))
    return FALSE;

  return TRUE;
}

gboolean
wyrebox_schema_migration_evaluate_to_current (WyreboxSchemaMigration *self,
    WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  return wyrebox_schema_migration_evaluate_to_version (self,
      state, wyrebox_schema_migration_get_current_schema_version (), error);
}
