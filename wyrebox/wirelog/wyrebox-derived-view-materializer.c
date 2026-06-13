#include "wyrebox-derived-view-materializer.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <string.h>

struct _WyreboxDerivedViewMaterializer
{
  GObject parent_instance;

  gchar *path;
  duckdb_database database;
  duckdb_connection connection;
};

G_DEFINE_TYPE (WyreboxDerivedViewMaterializer,
    wyrebox_derived_view_materializer, G_TYPE_OBJECT);

#define DERIVED_VIEW_UIDVALIDITY 1

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
is_non_empty (const gchar *value)
{
  return value != NULL && value[0] != '\0';
}

static gboolean
materializer_query (WyreboxDerivedViewMaterializer *self, const gchar *sql,
    GError **error)
{
  g_auto (duckdb_result) result = { 0 };

  if (duckdb_query (self->connection, sql, &result) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB derived view materializer query failed: %s",
      duckdb_result_error (&result) != NULL ?
      duckdb_result_error (&result) : sql);
  return FALSE;
}

static gboolean
materializer_prepare (WyreboxDerivedViewMaterializer *self, const gchar *sql,
    duckdb_prepared_statement *out_statement, GError **error)
{
  if (duckdb_prepare (self->connection, sql, out_statement) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB derived view materializer prepare failed: %s",
      *out_statement != NULL && duckdb_prepare_error (*out_statement) != NULL ?
      duckdb_prepare_error (*out_statement) : sql);
  return FALSE;
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
      "DuckDB derived view materializer string bind failed at index %"
      G_GUINT64_FORMAT, (guint64) index);
  return FALSE;
}

static gboolean
bind_uint64 (duckdb_prepared_statement statement, idx_t index, guint64 value,
    GError **error)
{
  if (duckdb_bind_uint64 (statement, index, (uint64_t) value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB derived view materializer uint64 bind failed at index %"
      G_GUINT64_FORMAT, (guint64) index);
  return FALSE;
}

static gboolean
materializer_execute_prepared (duckdb_prepared_statement statement,
    GError **error)
{
  g_auto (duckdb_result) result = { 0 };

  if (duckdb_execute_prepared (statement, &result) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB derived view materializer statement failed: %s",
      duckdb_result_error (&result) != NULL ?
      duckdb_result_error (&result) : "unknown DuckDB error");
  return FALSE;
}

static gboolean
materializer_count_prepared (duckdb_prepared_statement statement,
    guint64 *out_count, GError **error)
{
  g_auto (duckdb_result) result = { 0 };

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB derived view materializer count failed: %s",
        duckdb_result_error (&result) != NULL ?
        duckdb_result_error (&result) : "unknown DuckDB error");
    return FALSE;
  }

  if (duckdb_row_count (&result) != 1 || duckdb_column_count (&result) != 1 ||
      duckdb_value_is_null (&result, 0, 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB derived view materializer count returned malformed data");
    return FALSE;
  }

  *out_count = (guint64) duckdb_value_uint64 (&result, 0, 0);
  return TRUE;
}

static void
materializer_rollback_quietly (WyreboxDerivedViewMaterializer *self)
{
  g_auto (duckdb_result) result = { 0 };

  (void) duckdb_query (self->connection, "ROLLBACK;", &result);
}

static gboolean
materializer_insert_account (WyreboxDerivedViewMaterializer *self,
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
materializer_ensure_derived_view (WyreboxDerivedViewMaterializer *self,
    const gchar *account_id, const gchar *view_id, const gchar *imap_name,
    const gchar *definition_ref, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  guint64 count = 0;

  if (!materializer_prepare (self,
          "INSERT OR IGNORE INTO derived_views ("
          "view_id, account_id, imap_name, definition_ref, "
          "is_selectable, is_visible"
          ") VALUES (?, ?, ?, ?, TRUE, TRUE);", &statement, error) ||
      !bind_varchar (statement, 1, view_id, error) ||
      !bind_varchar (statement, 2, account_id, error) ||
      !bind_varchar (statement, 3, imap_name, error) ||
      !bind_varchar (statement, 4, definition_ref, error) ||
      !materializer_execute_prepared (statement, error))
    return FALSE;

  duckdb_destroy_prepare (&statement);
  if (!materializer_prepare (self,
          "SELECT COUNT(*) FROM derived_views WHERE view_id = ? "
          "AND account_id = ? AND imap_name = ? AND definition_ref = ? "
          "AND is_selectable = TRUE AND is_visible = TRUE;",
          &statement, error) ||
      !bind_varchar (statement, 1, view_id, error) ||
      !bind_varchar (statement, 2, account_id, error) ||
      !bind_varchar (statement, 3, imap_name, error) ||
      !bind_varchar (statement, 4, definition_ref, error) ||
      !materializer_count_prepared (statement, &count, error))
    return FALSE;

  if (count == 1)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "derived view %s does not match requested materialized state", view_id);
  return FALSE;
}

static gboolean
materializer_insert_uid_state (WyreboxDerivedViewMaterializer *self,
    const gchar *account_id, const gchar *view_id, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;

  return materializer_prepare (self,
      "INSERT OR IGNORE INTO mailbox_uid_state ("
      "account_id, namespace_kind, namespace_id, uidnext, uidvalidity"
      ") VALUES (?, 'derived_view', ?, 1, ?);", &statement, error)
      && bind_varchar (statement, 1, account_id, error)
      && bind_varchar (statement, 2, view_id, error)
      && bind_uint64 (statement, 3, DERIVED_VIEW_UIDVALIDITY, error)
      && materializer_execute_prepared (statement, error);
}

static gboolean
materializer_select_uidnext (WyreboxDerivedViewMaterializer *self,
    const gchar *account_id, const gchar *view_id, guint64 *out_uidnext,
    GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  guint64 uidnext = 0;
  guint64 uidvalidity = 0;
  guint64 max_uid = 0;

  if (!materializer_prepare (self,
          "SELECT uidnext, uidvalidity FROM mailbox_uid_state "
          "WHERE account_id = ? AND namespace_kind = 'derived_view' "
          "AND namespace_id = ?;", &statement, error) ||
      !bind_varchar (statement, 1, account_id, error) ||
      !bind_varchar (statement, 2, view_id, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB derived view materializer uidnext select failed: %s",
        duckdb_result_error (&result) != NULL ?
        duckdb_result_error (&result) : "unknown DuckDB error");
    return FALSE;
  }

  if (duckdb_row_count (&result) != 1 || duckdb_column_count (&result) != 2 ||
      duckdb_value_is_null (&result, 0, 0) ||
      duckdb_value_is_null (&result, 1, 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "derived view UID state is missing or malformed for %s/%s",
        account_id, view_id);
    return FALSE;
  }

  uidnext = (guint64) duckdb_value_uint64 (&result, 0, 0);
  uidvalidity = (guint64) duckdb_value_uint64 (&result, 1, 0);
  if (uidvalidity != DERIVED_VIEW_UIDVALIDITY) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "derived view UID state has unexpected UIDVALIDITY for %s/%s",
        account_id, view_id);
    return FALSE;
  }

  duckdb_destroy_prepare (&statement);
  if (!materializer_prepare (self,
          "SELECT COALESCE(MAX(uid), 0) FROM derived_view_memberships "
          "WHERE account_id = ? AND view_id = ?;", &statement, error) ||
      !bind_varchar (statement, 1, account_id, error) ||
      !bind_varchar (statement, 2, view_id, error) ||
      !materializer_count_prepared (statement, &max_uid, error))
    return FALSE;

  if (max_uid == G_MAXUINT64 || uidnext < max_uid + 1) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "derived view UID state is stale for %s/%s", account_id, view_id);
    return FALSE;
  }

  *out_uidnext = uidnext;
  return TRUE;
}

static gboolean
materializer_update_uidnext (WyreboxDerivedViewMaterializer *self,
    const gchar *account_id, const gchar *view_id, guint64 uidnext,
    GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;

  return materializer_prepare (self,
      "UPDATE mailbox_uid_state SET uidnext = ? "
      "WHERE account_id = ? AND namespace_kind = 'derived_view' "
      "AND namespace_id = ?;", &statement, error)
      && bind_uint64 (statement, 1, uidnext, error)
      && bind_varchar (statement, 2, account_id, error)
      && bind_varchar (statement, 3, view_id, error)
      && materializer_execute_prepared (statement, error);
}

static gboolean
materializer_validate_message_exists (WyreboxDerivedViewMaterializer *self,
    const gchar *account_id, const gchar *message_id, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  guint64 count = 0;

  if (!materializer_prepare (self,
          "SELECT COUNT(*) FROM messages WHERE account_id = ? "
          "AND message_id = ?;", &statement, error) ||
      !bind_varchar (statement, 1, account_id, error) ||
      !bind_varchar (statement, 2, message_id, error) ||
      !materializer_count_prepared (statement, &count, error))
    return FALSE;

  if (count == 1)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "derived view membership references unknown message %s", message_id);
  return FALSE;
}

static gboolean
materializer_membership_exists (WyreboxDerivedViewMaterializer *self,
    const gchar *account_id, const gchar *view_id, const gchar *message_id,
    const gchar *rule_version_hash, gboolean *out_exists, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  guint64 exact_count = 0;
  guint64 identity_count = 0;

  if (!materializer_prepare (self,
          "SELECT COUNT(*) FROM derived_view_memberships "
          "WHERE account_id = ? AND view_id = ? AND message_id = ? "
          "AND rule_version_hash = ? AND is_visible = TRUE;",
          &statement, error) ||
      !bind_varchar (statement, 1, account_id, error) ||
      !bind_varchar (statement, 2, view_id, error) ||
      !bind_varchar (statement, 3, message_id, error) ||
      !bind_varchar (statement, 4, rule_version_hash, error) ||
      !materializer_count_prepared (statement, &exact_count, error))
    return FALSE;

  if (exact_count == 1) {
    *out_exists = TRUE;
    return TRUE;
  }

  duckdb_destroy_prepare (&statement);
  if (!materializer_prepare (self,
          "SELECT COUNT(*) FROM derived_view_memberships "
          "WHERE view_id = ? AND message_id = ? AND rule_version_hash = ?;",
          &statement, error) ||
      !bind_varchar (statement, 1, view_id, error) ||
      !bind_varchar (statement, 2, message_id, error) ||
      !bind_varchar (statement, 3, rule_version_hash, error) ||
      !materializer_count_prepared (statement, &identity_count, error))
    return FALSE;

  if (identity_count == 0) {
    *out_exists = FALSE;
    return TRUE;
  }

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "derived view membership %s/%s does not match requested state",
      view_id, message_id);
  return FALSE;
}

static gchar *
materializer_build_membership_id (const gchar *view_id,
    const gchar *message_id, const gchar *rule_version_hash)
{
  g_autoptr (GChecksum) checksum = NULL;
  g_autofree gchar *digest = NULL;
  const gchar *components[] = {
    view_id,
    message_id,
    rule_version_hash,
  };

  checksum = g_checksum_new (G_CHECKSUM_SHA256);
  for (guint i = 0; i < G_N_ELEMENTS (components); i++) {
    const guint64 len = strlen (components[i]);
    guint8 len_bytes[8] = {
      (guint8) ((len >> 56) & 0xff),
      (guint8) ((len >> 48) & 0xff),
      (guint8) ((len >> 40) & 0xff),
      (guint8) ((len >> 32) & 0xff),
      (guint8) ((len >> 24) & 0xff),
      (guint8) ((len >> 16) & 0xff),
      (guint8) ((len >> 8) & 0xff),
      (guint8) (len & 0xff),
    };

    g_checksum_update (checksum, len_bytes, sizeof (len_bytes));
    g_checksum_update (checksum, (const guchar *) components[i], len);
  }

  digest = g_strdup (g_checksum_get_string (checksum));
  return g_strdup_printf ("derived-view:sha256:%s", digest);
}

static gboolean
materializer_insert_membership (WyreboxDerivedViewMaterializer *self,
    const gchar *account_id, const gchar *view_id, const gchar *message_id,
    const gchar *rule_version_hash, guint64 materialized_at_unix_us,
    guint64 uid, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_autofree gchar *membership_id = NULL;

  membership_id = materializer_build_membership_id (view_id, message_id,
      rule_version_hash);

  return materializer_prepare (self,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") VALUES (?, ?, ?, ?, ?, TRUE, ?, ?);", &statement, error)
      && bind_varchar (statement, 1, membership_id, error)
      && bind_varchar (statement, 2, account_id, error)
      && bind_varchar (statement, 3, view_id, error)
      && bind_varchar (statement, 4, message_id, error)
      && bind_uint64 (statement, 5, uid, error)
      && bind_varchar (statement, 6, rule_version_hash, error)
      && bind_uint64 (statement, 7, materialized_at_unix_us, error)
      && materializer_execute_prepared (statement, error);
}

static gboolean
materializer_apply_membership (WyreboxDerivedViewMaterializer *self,
    const gchar *account_id, const gchar *view_id,
    const gchar *rule_version_hash, guint64 materialized_at_unix_us,
    const WyreboxWirelogDerivedMembership *membership, guint64 *uidnext,
    GError **error)
{
  gboolean exists = FALSE;

  if (membership == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "derived view membership is required");
    return FALSE;
  }

  if (g_strcmp0 (membership->view_id, view_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view membership view_id does not match requested view");
    return FALSE;
  }

  if (!is_non_empty (membership->message_id)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view membership message_id is required");
    return FALSE;
  }

  if (!materializer_validate_message_exists (self, account_id,
          membership->message_id, error) ||
      !materializer_membership_exists (self, account_id, view_id,
          membership->message_id, rule_version_hash, &exists, error))
    return FALSE;

  if (exists)
    return TRUE;

  if (!materializer_insert_membership (self, account_id, view_id,
          membership->message_id, rule_version_hash, materialized_at_unix_us,
          *uidnext, error))
    return FALSE;

  (*uidnext)++;
  return TRUE;
}

static gboolean
materializer_validate_inputs (const gchar *account_id,
    const gchar *view_id, const gchar *imap_name, const gchar *definition_ref,
    const gchar *rule_version_hash, guint64 materialized_at_unix_us,
    GPtrArray *memberships, GError **error)
{
  g_autoptr (GHashTable) seen = NULL;

  if (!is_non_empty (account_id)) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "account_id is required");
    return FALSE;
  }

  if (!is_non_empty (view_id)) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "view_id is required");
    return FALSE;
  }

  if (!is_non_empty (imap_name)) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "imap_name is required");
    return FALSE;
  }

  if (!is_non_empty (definition_ref)) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "definition_ref is required");
    return FALSE;
  }

  if (!is_non_empty (rule_version_hash)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "rule_version_hash is required");
    return FALSE;
  }

  if (materialized_at_unix_us == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "materialization timestamp is required");
    return FALSE;
  }

  if (memberships == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "derived view memberships are required");
    return FALSE;
  }

  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < memberships->len; i++) {
    const WyreboxWirelogDerivedMembership *membership =
        g_ptr_array_index (memberships, i);
    g_autofree gchar *key = NULL;

    if (membership == NULL || !is_non_empty (membership->view_id) ||
        !is_non_empty (membership->message_id)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "derived view membership requires view_id and message_id");
      return FALSE;
    }

    key = g_strdup_printf ("%s\x1f%s\x1f%s", membership->view_id,
        membership->message_id, rule_version_hash);
    if (!g_hash_table_add (seen, g_steal_pointer (&key))) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "duplicate derived view membership input for %s/%s",
          membership->view_id, membership->message_id);
      return FALSE;
    }
  }

  return TRUE;
}

static void
wyrebox_derived_view_materializer_finalize (GObject *object)
{
  WyreboxDerivedViewMaterializer *self =
      WYREBOX_DERIVED_VIEW_MATERIALIZER (object);

  if (self->connection != NULL)
    duckdb_disconnect (&self->connection);
  if (self->database != NULL)
    duckdb_close (&self->database);
  g_clear_pointer (&self->path, g_free);

  G_OBJECT_CLASS (wyrebox_derived_view_materializer_parent_class)->finalize
      (object);
}

static void
    wyrebox_derived_view_materializer_class_init
    (WyreboxDerivedViewMaterializerClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_derived_view_materializer_finalize;
}

static void
wyrebox_derived_view_materializer_init (WyreboxDerivedViewMaterializer *self)
{
}

WyreboxDerivedViewMaterializer *
wyrebox_derived_view_materializer_new_duckdb (const gchar *path, GError **error)
{
  const gchar *effective_path = path;
  g_autoptr (WyreboxDerivedViewMaterializer) self = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!is_non_empty (path)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "DuckDB catalog path is required");
    return NULL;
  }

  if (g_strcmp0 (path, ":memory:") == 0)
    effective_path = NULL;

  self = g_object_new (WYREBOX_TYPE_DERIVED_VIEW_MATERIALIZER, NULL);
  self->path = g_strdup (path);

  if (duckdb_open (effective_path, &self->database) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "DuckDB derived view materializer open failed");
    return NULL;
  }

  if (duckdb_connect (self->database, &self->connection) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "DuckDB derived view materializer connect failed");
    return NULL;
  }

  return g_steal_pointer (&self);
}

gboolean
    wyrebox_derived_view_materializer_apply_memberships
    (WyreboxDerivedViewMaterializer * self, const gchar * account_id,
    const gchar * view_id, const gchar * imap_name,
    const gchar * definition_ref, const gchar * rule_version_hash,
    guint64 materialized_at_unix_us, GPtrArray * memberships, GError ** error)
{
  guint64 uidnext = 0;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!WYREBOX_IS_DERIVED_VIEW_MATERIALIZER (self)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view materializer instance is required");
    return FALSE;
  }

  if (!materializer_validate_inputs (account_id, view_id, imap_name,
          definition_ref, rule_version_hash, materialized_at_unix_us,
          memberships, error))
    return FALSE;

  if (!materializer_query (self, "BEGIN TRANSACTION;", error))
    return FALSE;

  if (!materializer_insert_account (self, account_id, error) ||
      !materializer_ensure_derived_view (self, account_id, view_id, imap_name,
          definition_ref, error) ||
      !materializer_insert_uid_state (self, account_id, view_id, error) ||
      !materializer_select_uidnext (self, account_id, view_id, &uidnext, error))
    goto fail;

  for (guint i = 0; i < memberships->len; i++) {
    const WyreboxWirelogDerivedMembership *membership =
        g_ptr_array_index (memberships, i);

    if (!materializer_apply_membership (self, account_id, view_id,
            rule_version_hash, materialized_at_unix_us, membership, &uidnext,
            error))
      goto fail;
  }

  if (!materializer_update_uidnext (self, account_id, view_id, uidnext, error))
    goto fail;

  if (!materializer_query (self, "COMMIT;", error))
    goto fail;

  return TRUE;

fail:
  materializer_rollback_quietly (self);
  return FALSE;
}
