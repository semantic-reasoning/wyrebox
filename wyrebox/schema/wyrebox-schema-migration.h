/*
 * Copyright (C) 2026
 */

#pragma once

#include <glib-object.h>
#include <gio/gio.h>

typedef struct _WyreboxJournalReader WyreboxJournalReader;
typedef struct _WyreboxSchemaMetadataStore WyreboxSchemaMetadataStore;

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * A migration path for legacy/raw schema metadata version transitions.
 */
#define WYREBOX_TYPE_SCHEMA_MIGRATION (wyrebox_schema_migration_get_type())

G_DECLARE_FINAL_TYPE (WyreboxSchemaMigration, wyrebox_schema_migration,
    WYREBOX, SCHEMA_MIGRATION, GObject)

/*
 * Operation callback for a migration step transition.
 */
typedef gboolean (*WyreboxSchemaMigrationStepOperationFunc) (guint64 source_version,
    guint64 target_version,
    GError **error);

/*
 * Validation callback for a migration step transition.
 */
typedef gboolean (*WyreboxSchemaMigrationStepValidationFunc) (guint64 source_version,
    guint64 target_version,
    GError **error);

/*
 * Fixture hook for operation/validation behavior overrides.
 *
 * Ownership:
 * - The object stores @user_data and uses @user_data_destroy to release it
 *   when hooks are replaced or the object is finalized.
 * - The fixture callback must remain valid for the hook lifetime.
 */
typedef gboolean (*WyreboxSchemaMigrationFixtureStepOperationFunc) (
    guint64 source_version,
    guint64 target_version,
    gpointer user_data,
    GError **error);
typedef gboolean (*WyreboxSchemaMigrationFixtureStepValidationFunc) (
    guint64 source_version,
    guint64 target_version,
    gpointer user_data,
    GError **error);

/*
 * Materialization checkpoint compatibility policy for an individual migration
 * step.
 */
typedef enum
{
  /*
   * Keep any previously known materialization checkpoint state.
   */
  WYREBOX_SCHEMA_MIGRATION_MATERIALIZATION_CHECKPOINT_PRESERVE,

  /*
   * Invalidate materialization checkpoint state after the step succeeds.
   */
  WYREBOX_SCHEMA_MIGRATION_MATERIALIZATION_CHECKPOINT_INVALIDATE,
} WyreboxSchemaMigrationMaterializationCheckpointPolicy;

/*
 * Optional metadata state for schema migrations.
 *
 * This structure intentionally models metadata storage as an in-memory value
 * object suitable for fixtures and deterministic tests.
 *
 * Ownership:
 * - The caller owns the structure.
 * - Ownership of fields is fully owned by the value itself (no heap pointers).
 */
typedef struct
{
  /*
   * TRUE when @schema_version is backed by metadata.
   */
  gboolean schema_version_present;

  /*
   * The current metadata version. Valid only when @schema_version_present is
   * TRUE.
   *
   * Ownership: value type field; owned by the struct.
   */
  guint64 schema_version;

  /*
   * TRUE when the caller has explicitly satisfied the in-memory snapshot/
   * checkpoint precondition for migration steps that require it.
   *
   * Ownership: value type field; owned by the struct.
   */
  gboolean checkpoint_precondition_satisfied;

  /*
   * TRUE when a materialization checkpoint payload is known in metadata.
   *
   * Ownership: value type field; owned by the struct.
   */
  gboolean materialization_checkpoint_present;

  /*
   * Durable replay byte offset for the current checkpoint payload.
   *
   * Ownership: value type field; owned by the struct.
   */
  guint64 materialization_checkpoint_journal_offset;

  /*
   * Durable replay sequence for the current checkpoint payload.
   *
   * Ownership: value type field; owned by the struct.
   */
  guint64 materialization_checkpoint_sequence;
} WyreboxSchemaMigrationMetadataState;

void wyrebox_schema_migration_metadata_state_clear (
    WyreboxSchemaMigrationMetadataState *state);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (
    WyreboxSchemaMigrationMetadataState,
    wyrebox_schema_migration_metadata_state_clear)

/*
 * Validate that the journal state is compatible with the migration metadata
 * checkpoint, if one is present.
 *
 * When @state->materialization_checkpoint_present is TRUE, the helper validates
 * that the journal has a safe prefix and that the checkpoint offset/sequence
 * still point at a readable record. The reader position is consumed by the
 * checkpoint seek and should be discarded by the caller after use.
 *
 * When no materialization checkpoint is recorded, the helper still validates
 * the journal prefix and returns success.
 */
gboolean wyrebox_schema_migration_validate_materialization_checkpoint (
    WyreboxJournalReader *journal_reader,
    const WyreboxSchemaMigrationMetadataState *state,
    GError **error);

/*
 * Returns the first supported schema version (small integer for fixture-safe
 * comparisons).
 */
guint64 wyrebox_schema_migration_get_first_supported_schema_version (void);

/*
 * Returns the current canonical schema version.
 */
guint64 wyrebox_schema_migration_get_current_schema_version (void);

/*
 * Install optional test-time hooks for step execution.
 *
 * Callers may use this to force fixture-only operation or validation failures
 * without external systems.
 */
void wyrebox_schema_migration_set_test_step_hooks (
    WyreboxSchemaMigration *self,
    WyreboxSchemaMigrationFixtureStepOperationFunc operation_hook,
    WyreboxSchemaMigrationFixtureStepValidationFunc validation_hook,
    gpointer operation_hook_user_data,
    GDestroyNotify operation_hook_user_data_destroy);

/*
 * Returns: (transfer full): a new plan evaluator.
 */
WyreboxSchemaMigration *wyrebox_schema_migration_new (void);

/*
 * Evaluate migration from @state->schema_version (or missing metadata) to
 * @target_version.
 *
 * If any visited migration step has @requires_checkpoint set, callers must set
 * @state->checkpoint_precondition_satisfied before evaluation and provide
 * materialization checkpoint metadata in @state, except when
 * @state->schema_version_present is FALSE. Missing metadata is treated as a
 * fresh-store bootstrap and synthesizes the bootstrap precondition/checkpoint
 * needed for the initial legacy step. Materialization checkpoint compatibility
 * policy is handled per step and applied only after operation and validation
 * succeed.
 *
 * If successful, transitions are applied in-memory and @state is updated.
 * On failure, @state is not modified.
 */
gboolean wyrebox_schema_migration_evaluate_to_version (
    WyreboxSchemaMigration *self,
    WyreboxSchemaMigrationMetadataState *state,
    guint64 target_version,
    GError **error);

/*
 * Convenience wrapper for evaluating to the current supported schema version.
 */
gboolean wyrebox_schema_migration_evaluate_to_current (
    WyreboxSchemaMigration *self,
    WyreboxSchemaMigrationMetadataState *state,
    GError **error);

/*
 * Load schema metadata from @metadata_store, apply a transient
 * materialization checkpoint precondition, evaluate to the current schema
 * version, and persist updated metadata only when evaluation succeeds with
 * durable-state changes. The transient
 * @checkpoint_precondition_satisfied flag is never persisted.
 *
 * Ownership:
 * - @self and @metadata_store are non-floating references owned by the caller.
 * - @error, when set, is a caller-owned GError; callers should clear it with
 *   g_clear_error() or use g_autoptr(GError).
 */
gboolean wyrebox_schema_migration_run_store_to_current (
    WyreboxSchemaMigration *self,
    WyreboxSchemaMetadataStore *metadata_store,
    gboolean checkpoint_precondition_satisfied,
    GError **error);

/*
 * Load schema metadata from @metadata_store, validate the supplied journal
 * against any recorded materialization checkpoint, apply the same transient
 * checkpoint precondition used by the basic helper, and persist updated
 * metadata only when evaluation succeeds with durable-state changes.
 *
 * Ownership:
 * - @self, @metadata_store, and @journal_reader are non-floating references
 *   owned by the caller.
 * - @error, when set, is a caller-owned GError; callers should clear it with
 *   g_clear_error() or use g_autoptr(GError).
 */
gboolean wyrebox_schema_migration_run_store_to_current_with_journal (
    WyreboxSchemaMigration *self,
    WyreboxSchemaMetadataStore *metadata_store,
    WyreboxJournalReader *journal_reader,
    gboolean checkpoint_precondition_satisfied,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
