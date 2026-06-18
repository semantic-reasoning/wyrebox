/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-migration.h"
#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-journal-reader.h"

#include <glib.h>
#include <string.h>

/*
 * Keep these small and explicit for fixture-backed deterministic transitions.
 */
#define WYREBOX_SCHEMA_VERSION_FIRST 1
#define WYREBOX_SCHEMA_VERSION_CURRENT 7
#define WYREBOX_SCHEMA_VERSION_LEGACY_0 0

typedef struct
{
  guint64 source_version;
  guint64 target_version;
  const gchar *name;
  WyreboxSchemaMetadataStoreMigrationOperation store_operation;
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

gboolean
    wyrebox_schema_migration_validate_materialization_checkpoint
    (WyreboxJournalReader * journal_reader,
    const WyreboxSchemaMigrationMetadataState * state, GError ** error)
{
  WyreboxJournalSafePrefix prefix = { 0 };

  g_return_val_if_fail (WYREBOX_IS_JOURNAL_READER (journal_reader), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_journal_reader_scan_safe_prefix (journal_reader, &prefix, error))
    return FALSE;

  if (prefix.unsafe_suffix_found) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "journal unsafe suffix found before checkpoint validation");
    return FALSE;
  }

  if (!state->materialization_checkpoint_present)
    return TRUE;

  if (!wyrebox_journal_reader_seek_after_checkpoint (journal_reader,
          state->materialization_checkpoint_journal_offset,
          state->materialization_checkpoint_sequence, error))
    return FALSE;

  return TRUE;
}

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
        WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
        wyrebox_schema_migration_default_step_operation,
        wyrebox_schema_migration_default_step_validation,
        TRUE,
      WYREBOX_SCHEMA_MIGRATION_MATERIALIZATION_CHECKPOINT_INVALIDATE},
  {WYREBOX_SCHEMA_VERSION_FIRST,
        2,
        "add-message-attribute-tables",
        WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES,
        wyrebox_schema_migration_default_step_operation,
        wyrebox_schema_migration_default_step_validation,
        FALSE,
      WYREBOX_SCHEMA_MIGRATION_MATERIALIZATION_CHECKPOINT_INVALIDATE},
  {2,
        3,
        "add-message-header-table",
        WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE,
        wyrebox_schema_migration_default_step_operation,
        wyrebox_schema_migration_default_step_validation,
        FALSE,
      WYREBOX_SCHEMA_MIGRATION_MATERIALIZATION_CHECKPOINT_INVALIDATE},
  {3,
        4,
        "add-derived-view-memberships",
        WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_DERIVED_VIEW_MEMBERSHIPS,
        wyrebox_schema_migration_default_step_operation,
        wyrebox_schema_migration_default_step_validation,
        FALSE,
      WYREBOX_SCHEMA_MIGRATION_MATERIALIZATION_CHECKPOINT_INVALIDATE},
  {4,
        5,
        "scope-derived-views-by-account",
        WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_SCOPE_DERIVED_VIEWS_BY_ACCOUNT,
        wyrebox_schema_migration_default_step_operation,
        wyrebox_schema_migration_default_step_validation,
        TRUE,
      WYREBOX_SCHEMA_MIGRATION_MATERIALIZATION_CHECKPOINT_INVALIDATE},
  {5,
        6,
        "add-message-header-sender-domain",
        WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_SENDER_DOMAIN,
        wyrebox_schema_migration_default_step_operation,
        wyrebox_schema_migration_default_step_validation,
        TRUE,
      WYREBOX_SCHEMA_MIGRATION_MATERIALIZATION_CHECKPOINT_INVALIDATE},
  {6,
        WYREBOX_SCHEMA_VERSION_CURRENT,
        "add-message-header-date-unix-us",
        WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_DATE_UNIX_US,
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
    WyreboxSchemaMetadataStore *metadata_store,
    WyreboxSchemaMigrationMetadataState *state,
    gboolean materialization_checkpoint_available, GError **error)
{
  if (step->requires_checkpoint && !state->checkpoint_precondition_satisfied) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "checkpoint precondition not satisfied for migration step %s",
        step->name);
    return FALSE;
  }

  if (step->requires_checkpoint && !materialization_checkpoint_available) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "materialization checkpoint metadata missing for migration step %s",
        step->name);
    return FALSE;
  }

  if (self->operation_hook != NULL &&
      !self->operation_hook (step->source_version, step->target_version,
          self->hook_user_data, error))
    return FALSE;

  if (metadata_store != NULL) {
    if (!wyrebox_schema_metadata_store_apply_migration_operation
        (metadata_store, step->store_operation, step->source_version,
            step->target_version, error))
      return FALSE;
  } else if (!step->operation (step->source_version, step->target_version,
          error)) {
    return FALSE;
  }

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
    WyreboxSchemaMetadataStore *metadata_store,
    guint64 source_version, guint64 target_version,
    gboolean materialization_checkpoint_available, GError **error)
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

    if (!wyrebox_schema_migration_apply_step (self,
            step, metadata_store, state,
            materialization_checkpoint_available, error))
      return FALSE;

    cursor = step->target_version;
  }

  return TRUE;
}

static gboolean
wyrebox_schema_migration_evaluate_to_version_internal (WyreboxSchemaMigration
    *self, WyreboxSchemaMetadataStore *metadata_store,
    WyreboxSchemaMigrationMetadataState *state, guint64 target_version,
    GError **error)
{
  guint64 source_version = 0;
  g_auto (WyreboxSchemaMigrationMetadataState) updated_state = { 0 };
  gboolean materialization_checkpoint_available = FALSE;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_MIGRATION (self), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  source_version = state->schema_version_present ? state->schema_version :
      WYREBOX_SCHEMA_VERSION_LEGACY_0;
  updated_state = *state;
  if (!state->schema_version_present && source_version ==
      WYREBOX_SCHEMA_VERSION_LEGACY_0) {
    updated_state.checkpoint_precondition_satisfied = TRUE;
    updated_state.materialization_checkpoint_present = TRUE;
  }
  materialization_checkpoint_available =
      updated_state.materialization_checkpoint_present;

  if (!metadata_source_is_supported (source_version, error))
    return FALSE;

  if (!metadata_target_is_supported (target_version, error))
    return FALSE;

  if (!metadata_target_is_not_downgrade (source_version, target_version, error))
    return FALSE;

  if (source_version < target_version &&
      !metadata_apply_to_version (self,
          &updated_state, metadata_store, source_version, target_version,
          materialization_checkpoint_available, error))
    return FALSE;

  updated_state.schema_version_present = TRUE;
  updated_state.schema_version = target_version;
  *state = updated_state;

  return TRUE;
}

gboolean
wyrebox_schema_migration_evaluate_to_version (WyreboxSchemaMigration *self,
    WyreboxSchemaMigrationMetadataState *state,
    guint64 target_version, GError **error)
{
  return wyrebox_schema_migration_evaluate_to_version_internal (self,
      NULL, state, target_version, error);
}

gboolean
wyrebox_schema_migration_evaluate_to_current (WyreboxSchemaMigration *self,
    WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  return wyrebox_schema_migration_evaluate_to_version (self, state,
      wyrebox_schema_migration_get_current_schema_version (), error);
}

static gboolean
wyrebox_schema_migration_state_matches_persisted_metadata (const
    WyreboxSchemaMigrationMetadataState *state,
    const WyreboxSchemaMigrationMetadataState *persisted_state)
{
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (persisted_state != NULL, FALSE);

  return state->schema_version_present ==
      persisted_state->schema_version_present
      && state->schema_version == persisted_state->schema_version
      && state->materialization_checkpoint_present ==
      persisted_state->materialization_checkpoint_present
      && state->materialization_checkpoint_journal_offset ==
      persisted_state->materialization_checkpoint_journal_offset
      && state->materialization_checkpoint_sequence ==
      persisted_state->materialization_checkpoint_sequence;
}

gboolean
wyrebox_schema_migration_run_store_to_current (WyreboxSchemaMigration *self,
    WyreboxSchemaMetadataStore *metadata_store,
    gboolean checkpoint_precondition_satisfied, GError **error)
{
  g_auto (WyreboxSchemaMigrationMetadataState) state = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) persisted_state = { 0 };

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_MIGRATION (self), FALSE);
  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (metadata_store),
      FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_schema_metadata_store_load (metadata_store, &state, error))
    return FALSE;

  persisted_state = state;
  state.checkpoint_precondition_satisfied = checkpoint_precondition_satisfied;

  if (!wyrebox_schema_migration_evaluate_to_version_internal (self,
          metadata_store,
          &state,
          wyrebox_schema_migration_get_current_schema_version (), error))
    return FALSE;

  if (wyrebox_schema_migration_state_matches_persisted_metadata (&state,
          &persisted_state))
    return TRUE;

  state.checkpoint_precondition_satisfied = FALSE;
  return wyrebox_schema_metadata_store_save (metadata_store, &state, error);
}
