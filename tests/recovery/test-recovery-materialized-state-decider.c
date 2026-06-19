#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-message-delivered-payload.h"
#include "wyrebox-delivery-replay-validator.h"
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

static gchar *
object_path_for_key (const char *root, const char *object_key)
{
  const char *hex = object_key + strlen ("sha256:");
  g_autofree char *prefix = g_strndup (hex, 2);
  g_autofree char *filename = g_strdup_printf ("%s.eml", hex);

  return g_build_filename (root, "objects", "sha256", prefix, filename, NULL);
}

static void
append_message_delivered_record (const char *journal_root,
    const char *object_store_root, guint64 *out_offset,
    guint64 *out_sequence, gchar **out_object_key)
{
  const guint8 message[] = "From: a@example.test\r\n\r\nhello\r\n";
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GBytes) input = g_bytes_new_static (message, sizeof (message) - 1);
  g_autoptr (GError) error = NULL;
  g_autofree char *object_key = NULL;
  g_autoptr (GBytes) payload = NULL;

  store = wyrebox_local_object_store_new (object_store_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_local_object_store_put_bytes (store, input,
          &object_key, &error));
  g_assert_no_error (error);

  payload = wyrebox_message_delivered_payload_encode (object_key,
      sizeof (message) - 1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (payload);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, payload, out_offset,
          out_sequence, &error));
  g_assert_no_error (error);

  if (out_object_key != NULL)
    *out_object_key = g_steal_pointer (&object_key);
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
assert_decision (WyreboxRecoveryMaterializedStateDecision actual,
    WyreboxRecoveryMaterializedStateDecision expected)
{
  g_assert_cmpint (actual, ==, expected);
}

static void
test_missing_catalog_triggers_rebuild (void)
{
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-recovery-object-store-XXXXXX", NULL);
  g_autofree char *catalog_path = create_temp_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxRecoveryMaterializedStateDecider) decider = NULL;
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  WyreboxRecoveryMaterializedStateDecision decision =
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_USE_MATERIALIZED_STATE;

  object_store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (object_store);
  (void) g_remove (catalog_path);

  decider = wyrebox_recovery_materialized_state_decider_new ();
  g_assert_true (wyrebox_recovery_materialized_state_decider_decide (decider,
          object_root, object_store, catalog_path, NULL, &metadata,
          &decision, &error));
  g_assert_no_error (error);
  assert_decision (decision,
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_REBUILD_MATERIALIZED_STATE);

  remove_tree (object_root);
}

static void
test_missing_checkpoint_triggers_rebuild (void)
{
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-recovery-object-store-XXXXXX", NULL);
  g_autofree char *catalog_path = create_temp_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxRecoveryMaterializedStateDecider) decider = NULL;
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  WyreboxRecoveryMaterializedStateDecision decision =
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_USE_MATERIALIZED_STATE;

  prepare_store (catalog_path, FALSE);
  object_store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (object_store);

  decider = wyrebox_recovery_materialized_state_decider_new ();
  g_assert_true (wyrebox_recovery_materialized_state_decider_decide (decider,
          object_root, object_store, catalog_path, NULL, &metadata,
          &decision, &error));
  g_assert_no_error (error);
  assert_decision (decision,
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
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  guint64 checkpoint_offset = 0;
  guint64 checkpoint_sequence = 0;
  WyreboxRecoveryMaterializedStateDecision decision =
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_REBUILD_MATERIALIZED_STATE;
  g_autofree gchar *object_key = NULL;

  append_message_delivered_record (journal_root, object_root,
      &checkpoint_offset, &checkpoint_sequence, &object_key);
  prepare_store (catalog_path, TRUE);
  object_store = wyrebox_local_object_store_open_existing (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (object_store);

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
          object_root, object_store, catalog_path, reader, &metadata,
          &decision, &error));
  g_assert_no_error (error);
  assert_decision (decision,
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_USE_MATERIALIZED_STATE);

  remove_tree (object_root);
  remove_tree (journal_root);
  (void) g_remove (catalog_path);
}

static void
test_missing_object_rejects_restore (void)
{
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-recovery-object-store-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-recovery-journal-XXXXXX", NULL);
  g_autofree char *catalog_path = create_temp_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxRecoveryMaterializedStateDecider) decider = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  guint64 checkpoint_offset = 0;
  guint64 checkpoint_sequence = 0;
  g_autofree gchar *object_key = NULL;
  g_autofree gchar *object_path = NULL;
  WyreboxRecoveryMaterializedStateDecision decision =
      WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_REBUILD_MATERIALIZED_STATE;

  append_message_delivered_record (journal_root, object_root,
      &checkpoint_offset, &checkpoint_sequence, &object_key);
  object_path = object_path_for_key (object_root, object_key);
  g_assert_true (g_remove (object_path) == 0);
  prepare_store (catalog_path, TRUE);
  object_store = wyrebox_local_object_store_open_existing (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (object_store);

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
  g_assert_false (wyrebox_recovery_materialized_state_decider_decide (decider,
          object_root, object_store, catalog_path, reader, &metadata,
          &decision, &error));
  g_assert_error (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
      WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_MISSING_OBJECT);

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
  g_test_add_func
      ("/recovery/materialized-state/missing-delivered-object-fails-before-rebuild",
      test_missing_object_rejects_restore);

  return g_test_run ();
}
