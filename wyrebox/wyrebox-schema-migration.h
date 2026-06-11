/*
 * Copyright (C) 2026
 */

#pragma once

#include <glib-object.h>
#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * A migration path for legacy/raw schema metadata version transitions.
 */
#define WYREBOX_TYPE_SCHEMA_MIGRATION (wyrebox_schema_migration_get_type())

G_DECLARE_FINAL_TYPE (WyreboxSchemaMigration, wyrebox_schema_migration,
    WYREBOX, SCHEMA_MIGRATION, GObject)

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
} WyreboxSchemaMigrationMetadataState;

void wyrebox_schema_migration_metadata_state_clear (
    WyreboxSchemaMigrationMetadataState *state);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (
    WyreboxSchemaMigrationMetadataState,
    wyrebox_schema_migration_metadata_state_clear)

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
 * Returns: (transfer full): a new plan evaluator.
 */
WyreboxSchemaMigration *wyrebox_schema_migration_new (void);

/*
 * Evaluate migration from @state->schema_version (or missing metadata) to
 * @target_version.
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

G_END_DECLS
/* *INDENT-ON* */
