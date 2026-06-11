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

struct _WyreboxSchemaMetadataStoreClass
{
  GObjectClass parent_class;

  gboolean (*load) (WyreboxSchemaMetadataStore *self,
      WyreboxSchemaMigrationMetadataState *out_state,
      GError **error);
  gboolean (*save) (WyreboxSchemaMetadataStore *self,
      const WyreboxSchemaMigrationMetadataState *state,
      GError **error);
};

/*
 * Construct an in-memory metadata store.
 *
 * Ownership: (transfer full) returned object is a floating GObject reference
 * until adopted by a new owner, typically by assigning to a g_autoptr slot.
 */
WyreboxSchemaMetadataStore *
wyrebox_schema_metadata_store_new_memory (void);

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
 * Test-only save-failure injection point for the in-memory implementation.
 * This causes the next save call to fail and leave existing persisted state
 * unchanged.
 */
void wyrebox_schema_metadata_store_memory_set_next_save_failure (
    WyreboxSchemaMetadataStore *self, gboolean fail_next_save);

G_END_DECLS
/* *INDENT-ON* */
