#include "wyrebox-recovery-materialized-state.h"

#include <glib/gstdio.h>

struct _WyreboxRecoveryMaterializedStateDecider
{
  GObject parent_instance;
};

G_DEFINE_TYPE (WyreboxRecoveryMaterializedStateDecider,
    wyrebox_recovery_materialized_state_decider, G_TYPE_OBJECT)
     static void
         wyrebox_recovery_materialized_state_decider_class_init
         (WyreboxRecoveryMaterializedStateDeciderClass *klass)
{
  (void) klass;
}

static void
    wyrebox_recovery_materialized_state_decider_init
    (WyreboxRecoveryMaterializedStateDecider * self)
{
  (void) self;
}

WyreboxRecoveryMaterializedStateDecider *
wyrebox_recovery_materialized_state_decider_new (void)
{
  return g_object_new (WYREBOX_TYPE_RECOVERY_MATERIALIZED_STATE_DECIDER, NULL);
}

static gboolean
path_exists (const gchar *path)
{
  return path != NULL && *path != '\0'
      && g_file_test (path, G_FILE_TEST_EXISTS);
}

gboolean
    wyrebox_recovery_materialized_state_decider_decide
    (WyreboxRecoveryMaterializedStateDecider * self,
    const gchar * object_store_root, const gchar * catalog_path,
    WyreboxJournalReader * journal_reader,
    const WyreboxSchemaMigrationMetadataState * metadata_state,
    WyreboxRecoveryMaterializedStateDecision * out_decision, GError ** error)
{
  g_return_val_if_fail (WYREBOX_IS_RECOVERY_MATERIALIZED_STATE_DECIDER (self),
      FALSE);
  g_return_val_if_fail (out_decision != NULL, FALSE);
  g_return_val_if_fail (metadata_state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!path_exists (object_store_root)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "object store root is missing");
    return FALSE;
  }

  if (!path_exists (catalog_path)) {
    *out_decision =
        WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_REBUILD_MATERIALIZED_STATE;
    return TRUE;
  }

  if (!metadata_state->materialization_checkpoint_present) {
    *out_decision =
        WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_REBUILD_MATERIALIZED_STATE;
    return TRUE;
  }

  if (journal_reader != NULL &&
      !wyrebox_schema_migration_validate_materialization_checkpoint
      (journal_reader, metadata_state, error))
    return FALSE;

  if (metadata_state->schema_version_present &&
      metadata_state->schema_version ==
      wyrebox_schema_migration_get_current_schema_version ()) {
    *out_decision =
        WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_USE_MATERIALIZED_STATE;
  } else {
    *out_decision =
        WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_REBUILD_MATERIALIZED_STATE;
  }

  return TRUE;
}
