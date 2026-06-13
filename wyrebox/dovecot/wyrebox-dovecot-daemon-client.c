#include "wyrebox-dovecot-daemon-client.h"

#include "wyrebox-build-config.h"
#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-duckdb-query-template-request.h"
#include "wyrebox-daemon-mailbox-select-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"
#include "wyrebox-daemon-uds-client.h"

#include <gio/gio.h>

#define WYREBOX_DOVECOT_CALLER_IDENTITY "dovecot"
#define WYREBOX_DOVECOT_TOOL_IDENTITY "dovecot-storage"
#define WYREBOX_DOVECOT_UID_MAP_TEMPLATE_ID "mailbox.uid_map.v1"

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION

static gboolean
validate_uid_map_inputs (const char *socket_path,
    const char *account_identity, const char *mailbox_id, GError **error)
{
  if (socket_path == NULL || socket_path[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires a non-empty socket path");
    return FALSE;
  }

  if (account_identity == NULL || account_identity[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires a non-empty account identity");
    return FALSE;
  }

  if (mailbox_id == NULL || mailbox_id[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires a non-empty mailbox id");
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_select_inputs (const char *socket_path,
    const char *account_identity, const char *mailbox_name, GError **error)
{
  if (socket_path == NULL || socket_path[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires a non-empty socket path");
    return FALSE;
  }

  if (account_identity == NULL || account_identity[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires a non-empty account identity");
    return FALSE;
  }

  if (mailbox_name == NULL || mailbox_name[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires a non-empty mailbox name");
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_response_request_id (const WyreboxDaemonResponseFrame *response,
    const char *request_id, GError **error)
{
  if (g_strcmp0 (response->request_id, request_id) == 0)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "daemon mailbox SELECT response request_id does not match request");
  return FALSE;
}

static gboolean
set_daemon_error_response (const WyreboxDaemonResponseFrame *response,
    GError **error)
{
  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "daemon mailbox SELECT failed: %s",
      response->error.message != NULL ? response->error.message :
      "daemon returned an error response");
  return FALSE;
}

static gboolean
copy_mailbox_select_response (const WyreboxDaemonResponseFrame *response,
    WyreboxDaemonMailboxSelectResult *out_result, GError **error)
{
  return wyrebox_daemon_mailbox_select_result_init (out_result,
      response->mailbox_select.kind,
      response->mailbox_select.mailbox_id,
      response->mailbox_select.mailbox_name,
      response->mailbox_select.uid_validity,
      response->mailbox_select.uid_next,
      response->mailbox_select.message_count, error);
}

void
wyrebox_dovecot_mailbox_uid_map_row_clear (WyreboxDovecotMailboxUidMapRow *row)
{
  if (row == NULL)
    return;

  g_clear_pointer (&row->account_id, g_free);
  g_clear_pointer (&row->mailbox_id, g_free);
  row->uid_validity = 0;
  row->uid = 0;
  g_clear_pointer (&row->message_id, g_free);
  g_clear_pointer (&row->object_id, g_free);
}

void
wyrebox_dovecot_mailbox_uid_map_row_free (WyreboxDovecotMailboxUidMapRow *row)
{
  if (row == NULL)
    return;

  wyrebox_dovecot_mailbox_uid_map_row_clear (row);
  g_free (row);
}

static void
wyrebox_dovecot_mailbox_uid_map_row_array_clear (gpointer data)
{
  g_ptr_array_unref (data);
}

void wyrebox_dovecot_mailbox_uid_map_snapshot_clear
    (WyreboxDovecotMailboxUidMapSnapshot * snapshot)
{
  if (snapshot == NULL)
    return;

  g_clear_pointer (&snapshot->rows,
      wyrebox_dovecot_mailbox_uid_map_row_array_clear);
}

static gboolean
is_empty_string (const char *value)
{
  return value == NULL || *value == '\0';
}

static gboolean
is_csv_newline (const char *csv, gsize len, gsize index, gsize *advance)
{
  if (csv[index] == '\n') {
    *advance = 1;
    return TRUE;
  }

  if (csv[index] == '\r') {
    *advance = (index + 1 < len && csv[index + 1] == '\n') ? 2 : 1;
    return TRUE;
  }

  return FALSE;
}

static void
append_field (GPtrArray *row, GString *field)
{
  g_ptr_array_add (row, g_strdup (field->str));
  g_string_truncate (field, 0);
}

static gboolean
parse_uid_map_uint64 (const char *value,
    const char *name, guint64 *out_value, GError **error)
{
  gchar *end = NULL;
  guint64 parsed = 0;

  if (is_empty_string (value)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "UID map CSV field %s must not be empty", name);
    return FALSE;
  }

  parsed = g_ascii_strtoull (value, &end, 10);
  if (end == NULL || *end != '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "UID map CSV field %s contains invalid integer: %s", name, value);
    return FALSE;
  }

  *out_value = parsed;
  return TRUE;
}

static gboolean
append_uid_map_row_from_csv_fields (GPtrArray *row_fields,
    const char *account_id,
    const char *mailbox_id,
    guint64 uid_validity, GPtrArray *rows, GError **error)
{
  const char *row_account_id = NULL;
  const char *row_mailbox_id = NULL;
  const char *row_uid_validity = NULL;
  const char *row_uid = NULL;
  const char *row_message_id = NULL;
  const char *row_object_id = NULL;
  WyreboxDovecotMailboxUidMapRow *row = NULL;
  guint64 parsed_uid_validity = 0;
  guint64 parsed_uid = 0;

  if (row_fields->len != 6) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "UID map CSV row must have exactly six columns");
    return FALSE;
  }

  row_account_id = g_ptr_array_index (row_fields, 0);
  row_mailbox_id = g_ptr_array_index (row_fields, 1);
  row_uid_validity = g_ptr_array_index (row_fields, 2);
  row_uid = g_ptr_array_index (row_fields, 3);
  row_message_id = g_ptr_array_index (row_fields, 4);
  row_object_id = g_ptr_array_index (row_fields, 5);

  if (g_strcmp0 (row_account_id, account_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "UID map row account_id mismatch: expected \"%s\" got \"%s\"",
        account_id, row_account_id);
    return FALSE;
  }

  if (g_strcmp0 (row_mailbox_id, mailbox_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "UID map row mailbox_id mismatch: expected \"%s\" got \"%s\"",
        mailbox_id, row_mailbox_id);
    return FALSE;
  }

  if (!parse_uid_map_uint64 (row_uid_validity,
          "uidvalidity", &parsed_uid_validity, error))
    return FALSE;

  if (parsed_uid_validity != uid_validity) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "UID map row uidvalidity \"%" G_GUINT64_FORMAT
        "\" does not match SELECT result", parsed_uid_validity);
    return FALSE;
  }

  if (!parse_uid_map_uint64 (row_uid, "uid", &parsed_uid, error))
    return FALSE;

  if (parsed_uid == 0) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "UID map row uid must be nonzero");
    return FALSE;
  }

  if (is_empty_string (row_message_id)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "UID map row message_id must not be empty");
    return FALSE;
  }

  if (is_empty_string (row_object_id)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "UID map row object_id must not be empty");
    return FALSE;
  }

  row = g_new0 (WyreboxDovecotMailboxUidMapRow, 1);
  row->account_id = g_strdup (row_account_id);
  row->mailbox_id = g_strdup (row_mailbox_id);
  row->uid_validity = parsed_uid_validity;
  row->uid = parsed_uid;
  row->message_id = g_strdup (row_message_id);
  row->object_id = g_strdup (row_object_id);
  g_ptr_array_add (rows, row);

  return TRUE;
}

static gboolean
append_uid_map_record (GPtrArray *row_fields,
    gboolean *header_seen,
    const char *account_id,
    const char *mailbox_id,
    guint64 uid_validity, GPtrArray *rows, GError **error)
{
  if (!*header_seen) {
    if (row_fields->len != 6
        || g_strcmp0 (g_ptr_array_index (row_fields, 0), "account_id") != 0
        || g_strcmp0 (g_ptr_array_index (row_fields, 1), "mailbox_id") != 0
        || g_strcmp0 (g_ptr_array_index (row_fields, 2), "uidvalidity") != 0
        || g_strcmp0 (g_ptr_array_index (row_fields, 3), "uid") != 0
        || g_strcmp0 (g_ptr_array_index (row_fields, 4), "message_id") != 0
        || g_strcmp0 (g_ptr_array_index (row_fields, 5), "object_id") != 0) {
      g_set_error (error,
          G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "UID map CSV header is invalid");
      return FALSE;
    }

    *header_seen = TRUE;
    return TRUE;
  }

  return append_uid_map_row_from_csv_fields (row_fields,
      account_id, mailbox_id, uid_validity, rows, error);
}

static gboolean
parse_uid_map_csv_rows (const char *csv,
    const char *account_id,
    const char *mailbox_id,
    guint64 uid_validity, GPtrArray *rows, GError **error)
{
  g_autoptr (GPtrArray) row_fields = NULL;
  g_autoptr (GString) field = NULL;
  gboolean in_quotes = FALSE;
  gboolean quoted_field = FALSE;
  gboolean after_quote = FALSE;
  gboolean have_field = FALSE;
  gboolean header_seen = FALSE;
  gsize len = 0;
  gsize i = 0;

  row_fields = g_ptr_array_new_with_free_func (g_free);
  field = g_string_new (NULL);
  len = strlen (csv);

  while (i < len) {
    gsize newline_advance = 0;
    char c = csv[i];

    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < len && csv[i + 1] == '"') {
          g_string_append_c (field, '"');
          i += 2;
        } else {
          in_quotes = FALSE;
          after_quote = TRUE;
          i++;
        }
      } else {
        g_string_append_c (field, c);
        i++;
      }
      continue;
    }

    if (after_quote) {
      if (c == ',') {
        append_field (row_fields, field);
        have_field = FALSE;
        quoted_field = FALSE;
        after_quote = FALSE;
        i++;
        continue;
      }

      if (is_csv_newline (csv, len, i, &newline_advance)) {
        append_field (row_fields, field);
        if (!append_uid_map_record (row_fields,
                &header_seen,
                account_id, mailbox_id, uid_validity, rows, error))
          return FALSE;
        g_ptr_array_set_size (row_fields, 0);
        have_field = FALSE;
        quoted_field = FALSE;
        after_quote = FALSE;
        i += newline_advance;
        continue;
      }

      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA, "Malformed UID map CSV quoted field");
      return FALSE;
    }

    if (c == ',' || is_csv_newline (csv, len, i, &newline_advance)) {
      append_field (row_fields, field);
      if (c != ',') {
        if (!append_uid_map_record (row_fields,
                &header_seen,
                account_id, mailbox_id, uid_validity, rows, error))
          return FALSE;
        g_ptr_array_set_size (row_fields, 0);
        i += newline_advance;
      } else {
        i++;
      }
      have_field = FALSE;
      quoted_field = FALSE;
      continue;
    }

    if (c == '"') {
      if (have_field || quoted_field) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Malformed UID map CSV quote");
        return FALSE;
      }

      quoted_field = TRUE;
      in_quotes = TRUE;
      i++;
      continue;
    }

    have_field = TRUE;
    g_string_append_c (field, c);
    i++;
  }

  if (in_quotes) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Malformed UID map CSV unterminated quoted field");
    return FALSE;
  }

  if (after_quote || have_field || quoted_field || row_fields->len > 0) {
    append_field (row_fields, field);
    if (!append_uid_map_record (row_fields,
            &header_seen, account_id, mailbox_id, uid_validity, rows, error))
      return FALSE;
  }

  if (!header_seen) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "UID map CSV is missing header row");
    return FALSE;
  }

  return TRUE;
}

static gboolean
decode_uid_map_response (const char *request_id,
    const char *account_id,
    const char *mailbox_id,
    guint64 uid_validity,
    const WyreboxDaemonResponseFrame *response,
    WyreboxDovecotMailboxUidMapSnapshot *snapshot, GError **error)
{
  const guint8 *csv_data = NULL;
  gsize csv_size = 0;
  g_autofree gchar *csv = NULL;

  if (response->kind != WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "daemon UID map response has unexpected kind");
    return FALSE;
  }

  if (g_strcmp0 (response->request_id, request_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon UID map response request_id does not match request");
    return FALSE;
  }

  if (response->stream_chunk.query_id == NULL
      || response->stream_chunk.query_id[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon UID map stream response must include query_id");
    return FALSE;
  }

  if (response->stream_chunk.chunk_index != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon UID map stream chunk index must start at zero");
    return FALSE;
  }

  if (!response->stream_chunk.end_of_stream) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon UID map response must return a single stream chunk");
    return FALSE;
  }

  if (response->stream_chunk.bytes == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "daemon UID map response payload is missing");
    return FALSE;
  }

  csv_data = g_bytes_get_data (response->stream_chunk.bytes, &csv_size);
  if (csv_data == NULL || csv_size == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "daemon UID map response payload is empty");
    return FALSE;
  }

  csv = g_strndup ((const char *) csv_data, csv_size);
  return parse_uid_map_csv_rows (csv,
      account_id, mailbox_id, uid_validity, snapshot->rows, error);
}

gboolean
wyrebox_dovecot_daemon_client_select_mailbox (const char *socket_path,
    const char *account_identity,
    const char *mailbox_name,
    WyreboxDaemonMailboxSelectResult *out_result, GError **error)
{
  g_autofree char *request_id = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response_payload = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (out_result == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires an output mailbox SELECT result");
    return FALSE;
  }

  wyrebox_daemon_mailbox_select_result_clear (out_result);

  if (!validate_select_inputs (socket_path, account_identity, mailbox_name,
          error))
    return FALSE;

  request_id = g_uuid_string_random ();
  if (!wyrebox_daemon_request_identity_init (&identity,
          request_id,
          WYREBOX_DOVECOT_CALLER_IDENTITY,
          account_identity, WYREBOX_DOVECOT_TOOL_IDENTITY, NULL, error))
    return FALSE;

  if (!wyrebox_daemon_mailbox_select_request_init (&request,
          account_identity, NULL, mailbox_name, error))
    return FALSE;

  request_payload =
      wyrebox_daemon_capnp_codec_encode_mailbox_select_request (&identity,
      &request, NULL, error);
  if (request_payload == NULL)
    return FALSE;

  response_payload =
      wyrebox_daemon_uds_client_send_request (socket_path, request_payload,
      error);
  if (response_payload == NULL)
    return FALSE;

  if (!wyrebox_daemon_capnp_codec_decode_response_frame (response_payload,
          &response, error))
    return FALSE;

  if (!validate_response_request_id (&response, request_id, error))
    return FALSE;

  switch (response.kind) {
    case WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_SELECT:
      return copy_mailbox_select_response (&response, out_result, error);
    case WYREBOX_DAEMON_RESPONSE_FRAME_ERROR:
      return set_daemon_error_response (&response, error);
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "daemon returned unexpected response kind %d for mailbox SELECT",
          response.kind);
      return FALSE;
  }
}

gboolean
wyrebox_dovecot_daemon_client_load_uid_map (const char *socket_path,
    const char *account_identity,
    const char *mailbox_id,
    guint64 uid_validity,
    WyreboxDovecotMailboxUidMapSnapshot *out_snapshot, GError **error)
{
  g_autofree char *query_id = NULL;
  g_autofree char *request_id = NULL;
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response_payload = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) parsed_snapshot = { 0 };
  const char *parameters[] = { mailbox_id, NULL };

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (out_snapshot == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot daemon client requires an output UID map snapshot");
    return FALSE;
  }

  if (!validate_uid_map_inputs (socket_path, account_identity, mailbox_id,
          error))
    return FALSE;

  wyrebox_dovecot_mailbox_uid_map_snapshot_clear (out_snapshot);

  request_id = g_uuid_string_random ();
  query_id = g_uuid_string_random ();

  if (!wyrebox_daemon_request_identity_init (&identity,
          request_id,
          WYREBOX_DOVECOT_CALLER_IDENTITY,
          account_identity, WYREBOX_DOVECOT_TOOL_IDENTITY, NULL, error))
    return FALSE;

  if (!wyrebox_daemon_duckdb_query_template_request_init (&request,
          query_id,
          WYREBOX_DOVECOT_UID_MAP_TEMPLATE_ID,
          account_identity, parameters, error))
    return FALSE;

  request_payload =
      wyrebox_daemon_capnp_codec_encode_duckdb_query_template_request
      (&identity, &request, NULL, error);
  if (request_payload == NULL)
    return FALSE;

  response_payload =
      wyrebox_daemon_uds_client_send_request (socket_path, request_payload,
      error);
  if (response_payload == NULL)
    return FALSE;

  if (!wyrebox_daemon_capnp_codec_decode_response_frame (response_payload,
          &response, error))
    return FALSE;

  parsed_snapshot.rows =
      g_ptr_array_new_with_free_func (
      (GDestroyNotify) wyrebox_dovecot_mailbox_uid_map_row_free);

  if (!decode_uid_map_response (request_id,
          account_identity,
          mailbox_id, uid_validity, &response, &parsed_snapshot, error)) {
    wyrebox_dovecot_mailbox_uid_map_snapshot_clear (&parsed_snapshot);
    return FALSE;
  }

  wyrebox_dovecot_mailbox_uid_map_snapshot_clear (out_snapshot);
  *out_snapshot = parsed_snapshot;
  parsed_snapshot.rows = NULL;
  return TRUE;
}

#else

gboolean
wyrebox_dovecot_daemon_client_select_mailbox (const char *socket_path,
    const char *account_identity,
    const char *mailbox_name,
    WyreboxDaemonMailboxSelectResult *out_result, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (out_result != NULL)
    wyrebox_daemon_mailbox_select_result_clear (out_result);

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "Dovecot daemon client requires Cap'n Proto serialization support");
  return FALSE;
}

gboolean
wyrebox_dovecot_daemon_client_load_uid_map (const char *socket_path,
    const char *account_identity,
    const char *mailbox_id,
    guint64 uid_validity,
    WyreboxDovecotMailboxUidMapSnapshot *out_snapshot, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  (void) socket_path;
  (void) account_identity;
  (void) mailbox_id;
  (void) uid_validity;

  if (out_snapshot != NULL)
    wyrebox_dovecot_mailbox_uid_map_snapshot_clear (out_snapshot);

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "Dovecot daemon client requires Cap'n Proto serialization support");
  return FALSE;
}

#endif
