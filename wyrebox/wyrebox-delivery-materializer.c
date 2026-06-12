#include "wyrebox-delivery-materializer.h"

#include <duckdb.h>
#include <gio/gio.h>

struct _WyreboxDeliveryMaterializer
{
  GObject parent_instance;

  gchar *path;
  duckdb_database database;
  duckdb_connection connection;
};

G_DEFINE_TYPE (WyreboxDeliveryMaterializer, wyrebox_delivery_materializer,
    G_TYPE_OBJECT);

#define MATERIALIZER_MAILBOX_UIDVALIDITY 1

static void
duckdb_result_clear (duckdb_result *result)
{
  duckdb_destroy_result (result);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, duckdb_result_clear)
/* *INDENT-ON* */

static void
duckdb_prepared_statement_clear (duckdb_prepared_statement *statement)
{
  duckdb_destroy_prepare (statement);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_prepared_statement,
    duckdb_prepared_statement_clear)
/* *INDENT-ON* */

static gboolean
materializer_query (WyreboxDeliveryMaterializer *self, const gchar *sql,
    GError **error)
{
  g_auto (duckdb_result) result = { 0 };

  if (duckdb_query (self->connection, sql, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB delivery materializer query failed: %s",
        detail != NULL ? detail : sql);
    return FALSE;
  }

  return TRUE;
}

static gboolean
materializer_prepare (WyreboxDeliveryMaterializer *self, const gchar *sql,
    duckdb_prepared_statement *out_statement, GError **error)
{
  if (duckdb_prepare (self->connection, sql, out_statement) != DuckDBSuccess) {
    const char *detail = *out_statement != NULL ?
        duckdb_prepare_error (*out_statement) : NULL;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB delivery materializer prepare failed: %s",
        detail != NULL ? detail : sql);
    return FALSE;
  }

  return TRUE;
}

static gboolean
materializer_execute_prepared (duckdb_prepared_statement statement,
    GError **error)
{
  g_auto (duckdb_result) result = { 0 };

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB delivery materializer statement failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  return TRUE;
}

static gboolean
materializer_count_prepared (duckdb_prepared_statement statement,
    guint64 *out_count, GError **error)
{
  g_auto (duckdb_result) result = { 0 };

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB delivery materializer count failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  if (duckdb_row_count (&result) != 1 || duckdb_column_count (&result) != 1 ||
      duckdb_value_is_null (&result, 0, 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB delivery materializer count returned malformed data");
    return FALSE;
  }

  *out_count = (guint64) duckdb_value_uint64 (&result, 0, 0);
  return TRUE;
}

static gboolean
bind_varchar (duckdb_prepared_statement statement, idx_t index,
    const gchar *value, GError **error)
{
  if (duckdb_bind_varchar (statement, index, value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB delivery materializer string bind failed at index %"
      G_GUINT64_FORMAT, (guint64) index);
  return FALSE;
}

static gboolean
bind_uint64 (duckdb_prepared_statement statement, idx_t index,
    guint64 value, GError **error)
{
  if (duckdb_bind_uint64 (statement, index, (uint64_t) value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB delivery materializer uint64 bind failed at index %"
      G_GUINT64_FORMAT, (guint64) index);
  return FALSE;
}

static gboolean
materializer_select_uidnext (WyreboxDeliveryMaterializer *self,
    const gchar *account_id, const gchar *mailbox_id, guint64 *out_uidnext,
    GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  idx_t row_count = 0;
  guint64 uidnext = 0;
  guint64 uidvalidity = 0;
  guint64 max_uid = 0;

  if (!materializer_prepare (self,
          "SELECT uidnext, uidvalidity FROM mailbox_uid_state "
          "WHERE account_id = ? AND namespace_kind = 'mailbox' "
          "AND namespace_id = ?;", &statement, error) ||
      !bind_varchar (statement, 1, account_id, error) ||
      !bind_varchar (statement, 2, mailbox_id, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB delivery materializer uidnext select failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  row_count = duckdb_row_count (&result);
  if (row_count != 1 || duckdb_column_count (&result) != 2 ||
      duckdb_value_is_null (&result, 0, 0) ||
      duckdb_value_is_null (&result, 1, 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "mailbox UID state is missing or malformed for %s/%s",
        account_id, mailbox_id);
    return FALSE;
  }

  uidnext = (guint64) duckdb_value_uint64 (&result, 0, 0);
  uidvalidity = (guint64) duckdb_value_uint64 (&result, 1, 0);
  if (uidvalidity != MATERIALIZER_MAILBOX_UIDVALIDITY) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "mailbox UID state has unexpected UIDVALIDITY for %s/%s",
        account_id, mailbox_id);
    return FALSE;
  }

  duckdb_destroy_prepare (&statement);
  if (!materializer_prepare (self,
          "SELECT COALESCE(MAX(uid), 0) FROM mailbox_memberships "
          "WHERE account_id = ? AND mailbox_id = ?;", &statement, error) ||
      !bind_varchar (statement, 1, account_id, error) ||
      !bind_varchar (statement, 2, mailbox_id, error) ||
      !materializer_count_prepared (statement, &max_uid, error))
    return FALSE;

  if (max_uid == G_MAXUINT64 || uidnext < max_uid + 1) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "mailbox UID state is stale for %s/%s", account_id, mailbox_id);
    return FALSE;
  }

  *out_uidnext = uidnext;
  return TRUE;
}

static gboolean
materializer_ensure_mailbox_exact (WyreboxDeliveryMaterializer *self,
    const gchar *account_id, const gchar *mailbox_id, const gchar *imap_name,
    GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  guint64 count = 0;

  if (!materializer_prepare (self,
          "SELECT COUNT(*) FROM mailboxes WHERE account_id = ? "
          "AND imap_name = ? AND mailbox_id <> ?;", &statement, error) ||
      !bind_varchar (statement, 1, account_id, error) ||
      !bind_varchar (statement, 2, imap_name, error) ||
      !bind_varchar (statement, 3, mailbox_id, error) ||
      !materializer_count_prepared (statement, &count, error))
    return FALSE;

  if (count != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "mailbox name %s/%s is already owned by another mailbox",
        account_id, imap_name);
    return FALSE;
  }

  duckdb_destroy_prepare (&statement);
  if (!materializer_prepare (self,
          "SELECT COUNT(*) FROM mailboxes WHERE mailbox_id = ? "
          "AND account_id = ? AND imap_name = ? "
          "AND is_selectable = TRUE AND is_visible = TRUE;",
          &statement, error) ||
      !bind_varchar (statement, 1, mailbox_id, error) ||
      !bind_varchar (statement, 2, account_id, error) ||
      !bind_varchar (statement, 3, imap_name, error) ||
      !materializer_count_prepared (statement, &count, error))
    return FALSE;

  if (count == 1)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "mailbox %s does not match requested materialized state", mailbox_id);
  return FALSE;
}

static gboolean
materializer_insert_account (WyreboxDeliveryMaterializer *self,
    const gchar *account_id, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;

  return materializer_prepare (self,
      "INSERT OR IGNORE INTO accounts (account_id) VALUES (?);",
      &statement, error)
      && bind_varchar (statement, 1, account_id, error)
      && materializer_execute_prepared (statement, error);
}

static gboolean
materializer_ensure_mailbox (WyreboxDeliveryMaterializer *self,
    const gchar *account_id, const gchar *mailbox_id, const gchar *imap_name,
    GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;

  return materializer_prepare (self,
      "INSERT OR IGNORE INTO mailboxes ("
      "mailbox_id, account_id, imap_name, is_selectable, is_visible"
      ") VALUES (?, ?, ?, TRUE, TRUE);", &statement, error)
      && bind_varchar (statement, 1, mailbox_id, error)
      && bind_varchar (statement, 2, account_id, error)
      && bind_varchar (statement, 3, imap_name, error)
      && materializer_execute_prepared (statement, error)
      && materializer_ensure_mailbox_exact (self, account_id, mailbox_id,
      imap_name, error);
}

static gboolean
materializer_insert_uid_state (WyreboxDeliveryMaterializer *self,
    const gchar *account_id, const gchar *mailbox_id, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;

  return materializer_prepare (self,
      "INSERT OR IGNORE INTO mailbox_uid_state ("
      "account_id, namespace_kind, namespace_id, uidnext, uidvalidity"
      ") VALUES (?, 'mailbox', ?, 1, ?);", &statement, error)
      && bind_varchar (statement, 1, account_id, error)
      && bind_varchar (statement, 2, mailbox_id, error)
      && bind_uint64 (statement, 3, MATERIALIZER_MAILBOX_UIDVALIDITY, error)
      && materializer_execute_prepared (statement, error);
}

static gboolean
materializer_ensure_object (WyreboxDeliveryMaterializer *self,
    const WyreboxDeliveryProjectionRecord *record, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  guint64 count = 0;

  if (!materializer_prepare (self,
          "INSERT OR IGNORE INTO objects (object_id, size_bytes) "
          "VALUES (?, ?);", &statement, error)
      || !bind_varchar (statement, 1, record->object_key, error)
      || !bind_uint64 (statement, 2, record->size_bytes, error)
      || !materializer_execute_prepared (statement, error))
    return FALSE;

  duckdb_destroy_prepare (&statement);
  if (!materializer_prepare (self,
          "SELECT COUNT(*) FROM objects WHERE object_id = ? "
          "AND size_bytes = ?;", &statement, error) ||
      !bind_varchar (statement, 1, record->object_key, error) ||
      !bind_uint64 (statement, 2, record->size_bytes, error) ||
      !materializer_count_prepared (statement, &count, error))
    return FALSE;

  if (count == 1)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "object %s does not match requested materialized state",
      record->object_key);
  return FALSE;
}

static gboolean
materializer_ensure_message (WyreboxDeliveryMaterializer *self,
    const gchar *account_id, const gchar *message_id,
    const WyreboxDeliveryProjectionRecord *record, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  guint64 count = 0;

  if (!materializer_prepare (self,
          "INSERT OR IGNORE INTO messages ("
          "message_id, account_id, object_id, journal_offset, journal_sequence"
          ") VALUES (?, ?, ?, ?, ?);", &statement, error)
      || !bind_varchar (statement, 1, message_id, error)
      || !bind_varchar (statement, 2, account_id, error)
      || !bind_varchar (statement, 3, record->object_key, error)
      || !bind_uint64 (statement, 4, record->journal_offset, error)
      || !bind_uint64 (statement, 5, record->journal_sequence, error)
      || !materializer_execute_prepared (statement, error))
    return FALSE;

  duckdb_destroy_prepare (&statement);
  if (!materializer_prepare (self,
          "SELECT COUNT(*) FROM messages WHERE message_id = ? "
          "AND account_id = ? AND object_id = ? AND journal_offset = ? "
          "AND journal_sequence = ?;", &statement, error) ||
      !bind_varchar (statement, 1, message_id, error) ||
      !bind_varchar (statement, 2, account_id, error) ||
      !bind_varchar (statement, 3, record->object_key, error) ||
      !bind_uint64 (statement, 4, record->journal_offset, error) ||
      !bind_uint64 (statement, 5, record->journal_sequence, error) ||
      !materializer_count_prepared (statement, &count, error))
    return FALSE;

  if (count == 1)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "message %s does not match requested materialized state", message_id);
  return FALSE;
}

static gboolean
materializer_ensure_membership_exact (WyreboxDeliveryMaterializer *self,
    const gchar *account_id, const gchar *mailbox_id,
    const gchar *message_id, const gchar *membership_id,
    const WyreboxDeliveryProjectionRecord *record, gboolean *out_exists,
    GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  guint64 count = 0;

  if (!materializer_prepare (self,
          "SELECT COUNT(*) FROM mailbox_memberships "
          "WHERE membership_id = ?;", &statement, error) ||
      !bind_varchar (statement, 1, membership_id, error) ||
      !materializer_count_prepared (statement, &count, error))
    return FALSE;

  if (count == 0) {
    *out_exists = FALSE;
    return TRUE;
  }

  duckdb_destroy_prepare (&statement);
  if (!materializer_prepare (self,
          "SELECT COUNT(*) FROM mailbox_memberships "
          "WHERE membership_id = ? AND account_id = ? AND mailbox_id = ? "
          "AND message_id = ? AND internal_date_unix_us = ? "
          "AND journal_offset = ? AND journal_sequence = ? "
          "AND is_visible = TRUE AND uid >= 1;", &statement, error) ||
      !bind_varchar (statement, 1, membership_id, error) ||
      !bind_varchar (statement, 2, account_id, error) ||
      !bind_varchar (statement, 3, mailbox_id, error) ||
      !bind_varchar (statement, 4, message_id, error) ||
      !bind_uint64 (statement, 5, record->internal_date_unix_us, error) ||
      !bind_uint64 (statement, 6, record->journal_offset, error) ||
      !bind_uint64 (statement, 7, record->journal_sequence, error) ||
      !materializer_count_prepared (statement, &count, error))
    return FALSE;

  if (count == 1) {
    *out_exists = TRUE;
    return TRUE;
  }

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "membership %s does not match requested materialized state",
      membership_id);
  return FALSE;
}

static gboolean
materializer_insert_membership (WyreboxDeliveryMaterializer *self,
    const gchar *account_id, const gchar *mailbox_id,
    const gchar *message_id, const gchar *membership_id,
    const WyreboxDeliveryProjectionRecord *record, guint64 uid, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;

  return materializer_prepare (self,
      "INSERT INTO mailbox_memberships ("
      "membership_id, account_id, mailbox_id, message_id, uid, "
      "internal_date_unix_us, journal_offset, journal_sequence, is_visible"
      ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, TRUE);", &statement, error)
      && bind_varchar (statement, 1, membership_id, error)
      && bind_varchar (statement, 2, account_id, error)
      && bind_varchar (statement, 3, mailbox_id, error)
      && bind_varchar (statement, 4, message_id, error)
      && bind_uint64 (statement, 5, uid, error)
      && bind_uint64 (statement, 6, record->internal_date_unix_us, error)
      && bind_uint64 (statement, 7, record->journal_offset, error)
      && bind_uint64 (statement, 8, record->journal_sequence, error)
      && materializer_execute_prepared (statement, error);
}

static gboolean
materializer_update_uidnext (WyreboxDeliveryMaterializer *self,
    const gchar *account_id, const gchar *mailbox_id, guint64 uidnext,
    GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;

  return materializer_prepare (self,
      "UPDATE mailbox_uid_state SET uidnext = ? "
      "WHERE account_id = ? AND namespace_kind = 'mailbox' "
      "AND namespace_id = ?;", &statement, error)
      && bind_uint64 (statement, 1, uidnext, error)
      && bind_varchar (statement, 2, account_id, error)
      && bind_varchar (statement, 3, mailbox_id, error)
      && materializer_execute_prepared (statement, error);
}

static void
materializer_rollback_quietly (WyreboxDeliveryMaterializer *self)
{
  g_auto (duckdb_result) result = { 0 };

  (void) duckdb_query (self->connection, "ROLLBACK;", &result);
}

static gboolean
materializer_apply_record (WyreboxDeliveryMaterializer *self,
    const gchar *account_id, const gchar *mailbox_id,
    const WyreboxDeliveryProjectionRecord *record, guint64 *uidnext,
    GError **error)
{
  g_autofree gchar *message_id = NULL;
  g_autofree gchar *membership_id = NULL;
  gboolean exists = FALSE;

  if (record == NULL || record->object_key == NULL ||
      record->object_key[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "delivery projection record requires a non-empty object key");
    return FALSE;
  }

  message_id = g_strdup_printf ("journal:%" G_GUINT64_FORMAT ":%"
      G_GUINT64_FORMAT, record->journal_offset, record->journal_sequence);
  membership_id = g_strdup_printf ("mailbox:%s:journal:%" G_GUINT64_FORMAT
      ":%" G_GUINT64_FORMAT,
      mailbox_id, record->journal_offset, record->journal_sequence);

  if (!materializer_ensure_object (self, record, error) ||
      !materializer_ensure_message (self, account_id, message_id, record,
          error) ||
      !materializer_ensure_membership_exact (self, account_id, mailbox_id,
          message_id, membership_id, record, &exists, error))
    return FALSE;

  if (exists)
    return TRUE;

  if (!materializer_insert_membership (self, account_id, mailbox_id,
          message_id, membership_id, record, *uidnext, error))
    return FALSE;

  (*uidnext)++;
  return TRUE;
}

static void
wyrebox_delivery_materializer_finalize (GObject *object)
{
  WyreboxDeliveryMaterializer *self = WYREBOX_DELIVERY_MATERIALIZER (object);

  if (self->connection != NULL)
    duckdb_disconnect (&self->connection);
  if (self->database != NULL)
    duckdb_close (&self->database);
  g_clear_pointer (&self->path, g_free);

  G_OBJECT_CLASS (wyrebox_delivery_materializer_parent_class)->finalize
      (object);
}

static void
wyrebox_delivery_materializer_class_init (WyreboxDeliveryMaterializerClass
    *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_delivery_materializer_finalize;
}

static void
wyrebox_delivery_materializer_init (WyreboxDeliveryMaterializer *self)
{
}

WyreboxDeliveryMaterializer *
wyrebox_delivery_materializer_new_duckdb (const gchar *path, GError **error)
{
  const gchar *effective_path = path;
  g_autoptr (WyreboxDeliveryMaterializer) self = NULL;

  g_return_val_if_fail (path != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (g_strcmp0 (path, ":memory:") == 0)
    effective_path = NULL;

  self = g_object_new (WYREBOX_TYPE_DELIVERY_MATERIALIZER, NULL);
  self->path = g_strdup (path);

  if (duckdb_open (effective_path, &self->database) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "DuckDB delivery materializer open failed");
    return NULL;
  }

  if (duckdb_connect (self->database, &self->connection) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "DuckDB delivery materializer connect failed");
    return NULL;
  }

  return g_steal_pointer (&self);
}

gboolean
wyrebox_delivery_materializer_apply_to_mailbox (WyreboxDeliveryMaterializer
    *self, const gchar *account_id, const gchar *mailbox_id,
    const gchar *imap_name, const WyreboxDeliveryProjectionList *projection,
    GError **error)
{
  guint64 uidnext = 0;

  g_return_val_if_fail (WYREBOX_IS_DELIVERY_MATERIALIZER (self), FALSE);
  g_return_val_if_fail (account_id != NULL, FALSE);
  g_return_val_if_fail (mailbox_id != NULL, FALSE);
  g_return_val_if_fail (imap_name != NULL, FALSE);
  g_return_val_if_fail (projection != NULL, FALSE);
  g_return_val_if_fail (projection->records != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!materializer_query (self, "BEGIN TRANSACTION;", error))
    return FALSE;

  if (!materializer_insert_account (self, account_id, error) ||
      !materializer_ensure_mailbox (self, account_id, mailbox_id, imap_name,
          error) ||
      !materializer_insert_uid_state (self, account_id, mailbox_id, error) ||
      !materializer_select_uidnext (self, account_id, mailbox_id, &uidnext,
          error))
    goto fail;

  for (guint i = 0; i < projection->records->len; i++) {
    const WyreboxDeliveryProjectionRecord *record =
        g_ptr_array_index (projection->records, i);

    if (!materializer_apply_record (self, account_id, mailbox_id, record,
            &uidnext, error))
      goto fail;
  }

  if (!materializer_update_uidnext (self, account_id, mailbox_id, uidnext,
          error) || !materializer_query (self, "COMMIT;", error))
    goto fail;

  return TRUE;

fail:
  materializer_rollback_quietly (self);
  return FALSE;
}
