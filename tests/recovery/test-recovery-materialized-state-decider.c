#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-recovery-materialized-state.h"
#include "wyrebox-schema-metadata-store.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

static gchar *
create_temp_catalog_path (void)
{
  gint fd = -1;
  gchar *path = NULL;

  fd = g_file_open_tmp ("wyrebox-recovery-catalog-XXXXXX.duckdb", &path, NULL);
  g_assert_cmpint (fd, >=, 0);
  g_assert_nonnull (path);

  g_assert_true (g_close (fd, NULL));
  (void) g_remove (path);
  return path;
}

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((entry = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, entry, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

static void
append_single_message_delivered_record (const char *root,
    guint64 *out_offset, guint64 *out_sequence)
{
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GBytes) payload = NULL;
  const guint8 bytes[] = { 0x01 };
  g_autoptr (GError) error = NULL;

  payload = g_bytes_new_static (bytes, sizeof (bytes));
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, payload, out_offset,
          out_sequence, &error));
  g_assert_no_error (error);
}

static void
prepare_store (const char *path, gboolean with_checkpoint)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  if (!with_checkpoint)
    return;

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation
      (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, 1, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation
      (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE,
          2, 3, &error));
  g_assert_no_error (error);
}

static void
test_missing_catalog_triggers_rebuild (void)
{
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-recovery-object-store-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-recovery-journal-XXXXXX", NULL);
  g_autofree char *catalog_path = create_temp_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxRecoveryMaterializedStateDecider) decider = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  WyreboxRecoveryMaterializedStateDecision decision =
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_USE_MATERIALIZED_STATE;

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);
  g_assert_nonnull (catalog_path);
  (void) g_remove (catalog_path);

  decider = wyrebox_recovery_materialized_state_decider_new ();
  g_assert_true (wyrebox_recovery_materialized_state_decider_decide (decider,
          object_root, catalog_path, NULL, &metadata, &decision, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decision, ==,
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_REBUILD_MATERIALIZED_STATE);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_missing_checkpoint_triggers_rebuild (void)
{
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-recovery-object-store-XXXXXX", NULL);
  g_autofree char *catalog_path = create_temp_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxRecoveryMaterializedStateDecider) decider = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  WyreboxRecoveryMaterializedStateDecision decision =
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_USE_MATERIALIZED_STATE;

  g_assert_nonnull (object_root);
  g_assert_nonnull (catalog_path);
  prepare_store (catalog_path, FALSE);

  decider = wyrebox_recovery_materialized_state_decider_new ();
  g_assert_true (wyrebox_recovery_materialized_state_decider_decide (decider,
          object_root, catalog_path, NULL, &metadata, &decision, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decision, ==,
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_REBUILD_MATERIALIZED_STATE);

  remove_tree (object_root);
  (void) g_remove (catalog_path);
}

static void
test_valid_checkpoint_uses_materialized_state (void)
{
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-recovery-object-store-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-recovery-journal-XXXXXX", NULL);
  g_autofree char *catalog_path = create_temp_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxRecoveryMaterializedStateDecider) decider = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  guint64 checkpoint_offset = 0;
  guint64 checkpoint_sequence = 0;
  WyreboxRecoveryMaterializedStateDecision decision =
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_REBUILD_MATERIALIZED_STATE;

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);
  g_assert_nonnull (catalog_path);

  append_single_message_delivered_record (journal_root, &checkpoint_offset,
      &checkpoint_sequence);
  prepare_store (catalog_path, TRUE);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  metadata.schema_version_present = TRUE;
  metadata.schema_version =
      wyrebox_schema_migration_get_current_schema_version ();
  metadata.materialization_checkpoint_present = TRUE;
  metadata.materialization_checkpoint_journal_offset = checkpoint_offset;
  metadata.materialization_checkpoint_sequence = checkpoint_sequence;

  decider = wyrebox_recovery_materialized_state_decider_new ();
  g_assert_true (wyrebox_recovery_materialized_state_decider_decide (decider,
          object_root, catalog_path, reader, &metadata, &decision, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decision, ==,
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_USE_MATERIALIZED_STATE);

  remove_tree (object_root);
  remove_tree (journal_root);
  (void) g_remove (catalog_path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/recovery/materialized-state/missing-catalog-rebuilds",
      test_missing_catalog_triggers_rebuild);
  g_test_add_func ("/recovery/materialized-state/missing-checkpoint-rebuilds",
      test_missing_checkpoint_triggers_rebuild);
  g_test_add_func ("/recovery/materialized-state/valid-checkpoint-uses",
      test_valid_checkpoint_uses_materialized_state);

  return g_test_run ();
}
