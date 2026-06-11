/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-migration.h"

#include <glib.h>
#include <string.h>

/*
 * Keep these small and explicit for fixture-backed deterministic transitions.
 */
#define WYREBOX_SCHEMA_VERSION_FIRST 1
#define WYREBOX_SCHEMA_VERSION_CURRENT 1
#define WYREBOX_SCHEMA_VERSION_LEGACY_0 0

typedef struct
{
  guint64 source_version;
  guint64 target_version;
  const gchar *name;
  WyreboxSchemaMigrationStepOperationFunc operation;
  WyreboxSchemaMigrationStepValidationFunc validate;
  gboolean requires_checkpoint;
  WyreboxSchemaMigrationMaterializationCheckpointPolicy checkpoint_policy;
} WyreboxSchemaMigrationStep;

struct _WyreboxSchemaMigration
{
  GObject parent_instance;

  WyreboxSchemaMigrationFixtureStepOperationFunc operation_hook;
  WyreboxSchemaMigrationFixtureStepValidationFunc validation_hook;
  gpointer hook_user_data;
  GDestroyNotify hook_user_data_destroy;
};

G_DEFINE_TYPE (WyreboxSchemaMigration, wyrebox_schema_migration, G_TYPE_OBJECT);

static gboolean
wyrebox_schema_migration_default_step_operation (guint64 source_version,
    guint64 target_version, GError **error)
{
  if (source_version == target_version) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "schema migration step must target a newer version");
    return FALSE;
  }

  return TRUE;
}

static gboolean
wyrebox_schema_migration_default_step_validation (guint64 source_version,
    guint64 target_version, GError **error)
{
  if (source_version == target_version) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "schema migration step must target a newer version");
    return FALSE;
  }

  return TRUE;
}

static const WyreboxSchemaMigrationStep wyrebox_schema_migration_steps[] = {
  {WYREBOX_SCHEMA_VERSION_LEGACY_0,
        WYREBOX_SCHEMA_VERSION_FIRST,
        "legacy-bootstrap",
        wyrebox_schema_migration_default_step_operation,
        wyrebox_schema_migration_default_step_validation,
        TRUE,
      WYREBOX_SCHEMA_MIGRATION_MATERIALIZATION_CHECKPOINT_INVALIDATE},
};

static void
    wyrebox_schema_migration_apply_step_checkpoint_policy
    (WyreboxSchemaMigrationMetadataState * state,
    const WyreboxSchemaMigrationStep * step)
{
  if (step->checkpoint_policy ==
      WYREBOX_SCHEMA_MIGRATION_MATERIALIZATION_CHECKPOINT_PRESERVE)
    return;

  state->materialization_checkpoint_present = FALSE;
  state->materialization_checkpoint_journal_offset = 0;
  state->materialization_checkpoint_sequence = 0;
}

static void
wyrebox_schema_migration_clear_test_step_hooks (WyreboxSchemaMigration *self)
{
  if (self->hook_user_data_destroy != NULL && self->hook_user_data != NULL)
    self->hook_user_data_destroy (self->hook_user_data);

  self->operation_hook = NULL;
  self->validation_hook = NULL;
  self->hook_user_data = NULL;
  self->hook_user_data_destroy = NULL;
}

void wyrebox_schema_migration_metadata_state_clear
    (WyreboxSchemaMigrationMetadataState * state)
{
  if (state == NULL)
    return;

  memset (state, 0, sizeof *state);
}

void
wyrebox_schema_migration_set_test_step_hooks (WyreboxSchemaMigration *self,
    WyreboxSchemaMigrationFixtureStepOperationFunc operation_hook,
    WyreboxSchemaMigrationFixtureStepValidationFunc validation_hook,
    gpointer operation_hook_user_data,
    GDestroyNotify operation_hook_user_data_destroy)
{
  g_return_if_fail (WYREBOX_IS_SCHEMA_MIGRATION (self));

  wyrebox_schema_migration_clear_test_step_hooks (self);

  self->operation_hook = operation_hook;
  self->validation_hook = validation_hook;
  self->hook_user_data = operation_hook_user_data;
  self->hook_user_data_destroy = operation_hook_user_data_destroy;
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
wyrebox_schema_migration_finalize (GObject *object)
{
  WyreboxSchemaMigration *self = WYREBOX_SCHEMA_MIGRATION (object);

  wyrebox_schema_migration_clear_test_step_hooks (self);

  G_OBJECT_CLASS (wyrebox_schema_migration_parent_class)->finalize (object);
}

static void
wyrebox_schema_migration_class_init (WyreboxSchemaMigrationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_schema_migration_finalize;
}

static void
wyrebox_schema_migration_init (WyreboxSchemaMigration *self)
{
  self->hook_user_data = NULL;
}

WyreboxSchemaMigration *
wyrebox_schema_migration_new (void)
{
  return g_object_new (WYREBOX_TYPE_SCHEMA_MIGRATION, NULL);
}

static gboolean
metadata_target_is_supported (guint64 target_version, GError **error)
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
metadata_source_is_supported (guint64 source_version, GError **error)
{
  guint64 current_version =
      wyrebox_schema_migration_get_current_schema_version ();

  if (source_version > current_version) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        "schema version %" G_GUINT64_FORMAT
        " is newer than current supported version %" G_GUINT64_FORMAT,
        source_version, current_version);
    return FALSE;
  }

  return TRUE;
}

static gboolean
metadata_target_is_not_downgrade (guint64 source_version,
    guint64 target_version, GError **error)
{
  if (target_version < source_version) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "target schema version %" G_GUINT64_FORMAT
        " would downgrade from %" G_GUINT64_FORMAT,
        target_version, source_version);
    return FALSE;
  }

  return TRUE;
}

static const WyreboxSchemaMigrationStep *
wyrebox_schema_migration_find_step_for_source (guint64 source_version)
{
  const WyreboxSchemaMigrationStep *step = NULL;

  for (gsize index = 0;
      index < G_N_ELEMENTS (wyrebox_schema_migration_steps); index++) {
    step = &wyrebox_schema_migration_steps[index];
    if (step->source_version == source_version)
      return step;
  }

  return NULL;
}

static gboolean
wyrebox_schema_migration_apply_step (WyreboxSchemaMigration *self,
    const WyreboxSchemaMigrationStep *step,
    WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  if (step->requires_checkpoint && !state->checkpoint_precondition_satisfied) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "checkpoint precondition not satisfied for migration step %s",
        step->name);
    return FALSE;
  }

  if (self->operation_hook != NULL &&
      !self->operation_hook (step->source_version, step->target_version,
          self->hook_user_data, error))
    return FALSE;

  if (!step->operation (step->source_version, step->target_version, error))
    return FALSE;

  if (self->validation_hook != NULL &&
      !self->validation_hook (step->source_version, step->target_version,
          self->hook_user_data, error))
    return FALSE;

  if (!step->validate (step->source_version, step->target_version, error))
    return FALSE;

  wyrebox_schema_migration_apply_step_checkpoint_policy (state, step);

  return TRUE;
}

static gboolean
metadata_apply_to_version (WyreboxSchemaMigration *self,
    WyreboxSchemaMigrationMetadataState *state,
    guint64 source_version, guint64 target_version, GError **error)
{
  guint64 cursor = source_version;

  while (cursor < target_version) {
    const WyreboxSchemaMigrationStep *step = NULL;

    step = wyrebox_schema_migration_find_step_for_source (cursor);
    if (step == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_NOT_SUPPORTED,
          "no migration path from schema version %" G_GUINT64_FORMAT
          " to %" G_GUINT64_FORMAT, source_version, target_version);
      return FALSE;
    }

    if (step->target_version <= cursor) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "migration step %s does not advance version", step->name);
      return FALSE;
    }

    if (step->target_version > target_version) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_NOT_SUPPORTED,
          "migration step %s overshoots target %" G_GUINT64_FORMAT,
          step->name, target_version);
      return FALSE;
    }

    if (!wyrebox_schema_migration_apply_step (self, step, state, error))
      return FALSE;

    cursor = step->target_version;
  }

  return TRUE;
}

gboolean
wyrebox_schema_migration_evaluate_to_version (WyreboxSchemaMigration *self,
    WyreboxSchemaMigrationMetadataState *state,
    guint64 target_version, GError **error)
{
  guint64 source_version = 0;
  g_auto (WyreboxSchemaMigrationMetadataState) updated_state = { 0 };

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_MIGRATION (self), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  source_version = state->schema_version_present ? state->schema_version :
      wyrebox_schema_migration_get_first_supported_schema_version ();
  updated_state = *state;

  if (!metadata_source_is_supported (source_version, error))
    return FALSE;

  if (!metadata_target_is_supported (target_version, error))
    return FALSE;

  if (!metadata_target_is_not_downgrade (source_version, target_version, error))
    return FALSE;

  if (source_version < target_version &&
      !metadata_apply_to_version (self,
          &updated_state, source_version, target_version, error))
    return FALSE;

  updated_state.schema_version_present = TRUE;
  updated_state.schema_version = target_version;
  *state = updated_state;

  return TRUE;
}

gboolean
wyrebox_schema_migration_evaluate_to_current (WyreboxSchemaMigration *self,
    WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  return wyrebox_schema_migration_evaluate_to_version (self, state,
      wyrebox_schema_migration_get_current_schema_version (), error);
}
