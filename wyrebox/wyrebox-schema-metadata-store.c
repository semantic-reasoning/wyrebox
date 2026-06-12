/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-metadata-store.h"

#include <glib.h>

typedef struct
{
  WyreboxSchemaMetadataStore parent_instance;

  gboolean has_state;
  WyreboxSchemaMigrationMetadataState persisted_state;
  gboolean fail_next_save;
} WyreboxSchemaMetadataStoreMemory;

typedef struct
{
  WyreboxSchemaMetadataStoreClass parent_class;
} WyreboxSchemaMetadataStoreMemoryClass;

G_DEFINE_TYPE (WyreboxSchemaMetadataStore, wyrebox_schema_metadata_store,
    G_TYPE_OBJECT);

static gboolean
wyrebox_schema_metadata_store_default_load (WyreboxSchemaMetadataStore *self,
    WyreboxSchemaMigrationMetadataState *out_state, GError **error)
{
  (void) self;
  (void) out_state;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "schema metadata store load is not implemented");
  return FALSE;
}

static gboolean
wyrebox_schema_metadata_store_default_save (WyreboxSchemaMetadataStore *self,
    const WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  (void) self;
  (void) state;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "schema metadata store save is not implemented");
  return FALSE;
}

static void
wyrebox_schema_metadata_store_init (WyreboxSchemaMetadataStore *self)
{
}

static void
wyrebox_schema_metadata_store_class_init (WyreboxSchemaMetadataStoreClass
    *klass)
{
  klass->load = wyrebox_schema_metadata_store_default_load;
  klass->save = wyrebox_schema_metadata_store_default_save;
}

G_DEFINE_TYPE (WyreboxSchemaMetadataStoreMemory,
    wyrebox_schema_metadata_store_memory, WYREBOX_TYPE_SCHEMA_METADATA_STORE);

static gboolean
wyrebox_schema_metadata_store_memory_load (WyreboxSchemaMetadataStore *self,
    WyreboxSchemaMigrationMetadataState *out_state, GError **error)
{
  WyreboxSchemaMetadataStoreMemory *memory_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_memory_get_type ()), FALSE);
  g_return_val_if_fail (out_state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  memory_store = (WyreboxSchemaMetadataStoreMemory *) self;
  wyrebox_schema_migration_metadata_state_clear (out_state);

  if (!memory_store->has_state)
    return TRUE;

  *out_state = memory_store->persisted_state;
  out_state->checkpoint_precondition_satisfied = FALSE;
  return TRUE;
}

static gboolean
wyrebox_schema_metadata_store_memory_save (WyreboxSchemaMetadataStore *self,
    const WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  WyreboxSchemaMetadataStoreMemory *memory_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_memory_get_type ()), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  memory_store = (WyreboxSchemaMetadataStoreMemory *) self;
  if (memory_store->fail_next_save) {
    memory_store->fail_next_save = FALSE;
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_FAILED, "forced metadata store save failure");
    return FALSE;
  }

  memory_store->persisted_state = *state;
  memory_store->persisted_state.checkpoint_precondition_satisfied = FALSE;
  memory_store->has_state = TRUE;
  return TRUE;
}

static void
    wyrebox_schema_metadata_store_memory_class_init
    (WyreboxSchemaMetadataStoreMemoryClass * klass)
{
  WyreboxSchemaMetadataStoreClass *store_class =
      WYREBOX_SCHEMA_METADATA_STORE_CLASS (klass);

  store_class->load = wyrebox_schema_metadata_store_memory_load;
  store_class->save = wyrebox_schema_metadata_store_memory_save;
}

static void
wyrebox_schema_metadata_store_memory_init (WyreboxSchemaMetadataStoreMemory
    *self)
{
  self->fail_next_save = FALSE;
  self->has_state = FALSE;
  wyrebox_schema_migration_metadata_state_clear (&self->persisted_state);
}

gboolean
wyrebox_schema_metadata_store_load (WyreboxSchemaMetadataStore *self,
    WyreboxSchemaMigrationMetadataState *out_state, GError **error)
{
  WyreboxSchemaMetadataStoreClass *klass = NULL;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self), FALSE);
  g_return_val_if_fail (out_state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  klass = WYREBOX_SCHEMA_METADATA_STORE_GET_CLASS (self);
  if (klass == NULL || klass->load == NULL)
    return FALSE;

  return klass->load (self, out_state, error);
}

gboolean
wyrebox_schema_metadata_store_save (WyreboxSchemaMetadataStore *self,
    const WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  WyreboxSchemaMetadataStoreClass *klass = NULL;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  klass = WYREBOX_SCHEMA_METADATA_STORE_GET_CLASS (self);
  if (klass == NULL || klass->save == NULL)
    return FALSE;

  return klass->save (self, state, error);
}

WyreboxSchemaMetadataStore *
wyrebox_schema_metadata_store_new_memory (void)
{
  return g_object_new (wyrebox_schema_metadata_store_memory_get_type (), NULL);
}

WyreboxSchemaMetadataStore *
wyrebox_schema_metadata_store_new_duckdb (const gchar *path, GError **error)
{
  g_return_val_if_fail (path != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  (void) path;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "DuckDB schema metadata store is not implemented yet");

  return NULL;
}

void wyrebox_schema_metadata_store_memory_set_next_save_failure
    (WyreboxSchemaMetadataStore * self, gboolean fail_next_save)
{
  WyreboxSchemaMetadataStoreMemory *memory_store = NULL;

  g_return_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self));
  g_return_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_memory_get_type ()));

  memory_store = (WyreboxSchemaMetadataStoreMemory *) self;
  memory_store->fail_next_save = fail_next_save;
}
