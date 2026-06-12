/*
 * Copyright (C) 2026
 */

#pragma once

#include <glib-object.h>

#include "wyrebox-schema-migration.h"

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_SCHEMA_METADATA_STORE (wyrebox_schema_metadata_store_get_type())

G_DECLARE_DERIVABLE_TYPE (WyreboxSchemaMetadataStore,
    wyrebox_schema_metadata_store,
    WYREBOX,
    SCHEMA_METADATA_STORE,
    GObject)

typedef enum
{
  WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP = 1,
  WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES,
} WyreboxSchemaMetadataStoreMigrationOperation;

struct _WyreboxSchemaMetadataStoreClass
{
  GObjectClass parent_class;

  gboolean (*load) (WyreboxSchemaMetadataStore *self,
      WyreboxSchemaMigrationMetadataState *out_state,
      GError **error);
  gboolean (*save) (WyreboxSchemaMetadataStore *self,
      const WyreboxSchemaMigrationMetadataState *state,
      GError **error);
  gboolean (*apply_migration_operation) (
      WyreboxSchemaMetadataStore *self,
      WyreboxSchemaMetadataStoreMigrationOperation operation,
      guint64 source_version,
      guint64 target_version,
      GError **error);
};

/*
 * Construct an in-memory metadata store.
 *
 * Returns: (transfer full): a non-floating GObject reference owned by the
 * caller, typically stored in a g_autoptr slot.
 */
WyreboxSchemaMetadataStore *
wyrebox_schema_metadata_store_new_memory (void);

/*
 * Construct a DuckDB-backed metadata store for @path.
 *
 * Ownership/error behavior:
 * - @path must be non-NULL and is not retained by this call.
 * - On success, returns a non-floating GObject reference owned by the caller.
 * - On failure, returns NULL and sets @error when provided.
 *
 * The DuckDB implementation persists schema metadata and materialization
 * checkpoint state. It does not fall back to the in-memory store.
 *
 * Returns: (transfer full): a non-floating GObject reference owned by the
 * caller, or NULL with @error set.
 */
WyreboxSchemaMetadataStore *
wyrebox_schema_metadata_store_new_duckdb (const gchar *path, GError **error);

/*
 * Load the persisted schema metadata state into @out_state.
 *
 * On success, @out_state is always initialized. If nothing has been saved
 * yet, @out_state->schema_version_present is FALSE and this should be treated as
 * missing metadata. Other fields are set to a cleared, deterministic default.
 */
gboolean wyrebox_schema_metadata_store_load (
    WyreboxSchemaMetadataStore *self,
    WyreboxSchemaMigrationMetadataState *out_state,
    GError **error);

/*
 * Save @state as persisted schema metadata.
 *
 * The in-memory store does not persist @state->checkpoint_precondition_satisfied;
 * load should therefore always return FALSE for that field.
 */
gboolean wyrebox_schema_metadata_store_save (
    WyreboxSchemaMetadataStore *self,
    const WyreboxSchemaMigrationMetadataState *state,
    GError **error);

/*
 * Apply a typed physical migration operation against the backing metadata
 * store. This does not accept caller-provided SQL or arbitrary DuckDB handles;
 * operation identifiers are owned by WyreBox schema migration code.
 */
gboolean wyrebox_schema_metadata_store_apply_migration_operation (
    WyreboxSchemaMetadataStore *self,
    WyreboxSchemaMetadataStoreMigrationOperation operation,
    guint64 source_version,
    guint64 target_version,
    GError **error);

/*
 * Test-only save-failure injection point for the in-memory implementation.
 * This causes the next save call to fail and leave existing persisted state
 * unchanged.
 */
void wyrebox_schema_metadata_store_memory_set_next_save_failure (
    WyreboxSchemaMetadataStore *self, gboolean fail_next_save);

G_END_DECLS
/* *INDENT-ON* */
