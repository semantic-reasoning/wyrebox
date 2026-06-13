#include "wyrebox-daemon-audit-payload.h"

#include <gio/gio.h>
#include <string.h>

#define WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_V1 "WYREDAU1"
#define WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_V2 "WYREDAU2"
#define WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_LEN 8
#define WYREBOX_DAEMON_AUDIT_PAYLOAD_HEADER_SIZE_V1 34
#define WYREBOX_DAEMON_AUDIT_PAYLOAD_HEADER_SIZE_V2 42
#define WYREBOX_DAEMON_AUDIT_PAYLOAD_NULL_STRING G_MAXUINT32

static inline void
write_u32_le (guint8 *dst, guint32 value)
{
  dst[0] = (guint8) ((value >> 0) & 0xff);
  dst[1] = (guint8) ((value >> 8) & 0xff);
  dst[2] = (guint8) ((value >> 16) & 0xff);
  dst[3] = (guint8) ((value >> 24) & 0xff);
}

static inline void
write_u64_le (guint8 *dst, guint64 value)
{
  dst[0] = (guint8) ((value >> 0) & 0xff);
  dst[1] = (guint8) ((value >> 8) & 0xff);
  dst[2] = (guint8) ((value >> 16) & 0xff);
  dst[3] = (guint8) ((value >> 24) & 0xff);
  dst[4] = (guint8) ((value >> 32) & 0xff);
  dst[5] = (guint8) ((value >> 40) & 0xff);
  dst[6] = (guint8) ((value >> 48) & 0xff);
  dst[7] = (guint8) ((value >> 56) & 0xff);
}

static inline guint32
read_u32_le (const guint8 *buffer)
{
  return (guint32) buffer[0] |
      ((guint32) buffer[1] << 8) |
      ((guint32) buffer[2] << 16) | ((guint32) buffer[3] << 24);
}

static inline guint64
read_u64_le (const guint8 *buffer)
{
  return (guint64) buffer[0] |
      ((guint64) buffer[1] << 8) |
      ((guint64) buffer[2] << 16) |
      ((guint64) buffer[3] << 24) |
      ((guint64) buffer[4] << 32) |
      ((guint64) buffer[5] << 40) |
      ((guint64) buffer[6] << 48) | ((guint64) buffer[7] << 56);
}

static gboolean
checked_add_size (gsize *total, gsize value, GError **error)
{
  if (value > G_MAXSIZE - *total) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "DaemonAuditRecorded payload length overflows memory");
    return FALSE;
  }

  *total += value;
  return TRUE;
}

static gboolean
validate_required_string (const char *field_name, const char *value,
    GIOErrorEnum code, GError **error)
{
  if (value == NULL || value[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        code, "DaemonAuditRecorded payload %s is required", field_name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_operation (WyreboxDaemonAuditOperation operation,
    GIOErrorEnum code, GError **error)
{
  if (operation == WYREBOX_DAEMON_AUDIT_OPERATION_SINGLE_FACT_MUTATION ||
      operation == WYREBOX_DAEMON_AUDIT_OPERATION_FACT_BATCH_IMPORT ||
      operation == WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE ||
      operation == WYREBOX_DAEMON_AUDIT_OPERATION_WIRELOG_PREDICATE_QUERY)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR, code, "DaemonAuditRecorded payload operation is invalid");
  return FALSE;
}

static gboolean
validate_outcome (WyreboxDaemonAuditOutcome outcome,
    GIOErrorEnum code, GError **error)
{
  if (outcome == WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS ||
      outcome == WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR, code, "DaemonAuditRecorded payload outcome is invalid");
  return FALSE;
}

static gboolean
validate_payload (const WyreboxDaemonAuditPayload *payload,
    GIOErrorEnum code, GError **error)
{
  if (payload == NULL) {
    g_set_error (error,
        G_IO_ERROR, code, "DaemonAuditRecorded payload is required");
    return FALSE;
  }

  if (!validate_operation (payload->operation, code, error) ||
      !validate_outcome (payload->outcome, code, error) ||
      !validate_required_string ("request_id", payload->request_id, code,
          error) ||
      !validate_required_string ("caller_identity", payload->caller_identity,
          code, error) ||
      !validate_required_string ("account_identity", payload->account_identity,
          code, error) ||
      !validate_required_string ("scope_id", payload->scope_id, code, error))
    return FALSE;

  if (payload->operation ==
      WYREBOX_DAEMON_AUDIT_OPERATION_SINGLE_FACT_MUTATION &&
      !validate_required_string ("predicate_id", payload->predicate_id, code,
          error))
    return FALSE;

  if (payload->operation ==
      WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE ||
      payload->operation ==
      WYREBOX_DAEMON_AUDIT_OPERATION_WIRELOG_PREDICATE_QUERY) {
    if (payload->outcome != WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE) {
      g_set_error (error,
          G_IO_ERROR,
          code, "DaemonAuditRecorded query audit outcome must be failure");
      return FALSE;
    }

    if (!validate_required_string ("query_id", payload->query_id, code,
            error) ||
        !validate_required_string ("template_id", payload->template_id, code,
            error) ||
        !validate_required_string ("error_domain", payload->error_domain,
            code, error) ||
        !validate_required_string ("error_class", payload->error_class, code,
            error) ||
        !validate_required_string ("error_message", payload->error_message,
            code, error))
      return FALSE;
  }

  if (payload->operation !=
      WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE &&
      payload->operation !=
      WYREBOX_DAEMON_AUDIT_OPERATION_WIRELOG_PREDICATE_QUERY &&
      payload->mutation_count == 0) {
    g_set_error (error,
        G_IO_ERROR, code, "DaemonAuditRecorded mutation_count is required");
    return FALSE;
  }

  if (payload->operation ==
      WYREBOX_DAEMON_AUDIT_OPERATION_SINGLE_FACT_MUTATION &&
      payload->mutation_count != 1) {
    g_set_error (error,
        G_IO_ERROR,
        code, "DaemonAuditRecorded single fact mutation count must be one");
    return FALSE;
  }

  if (payload->operation !=
      WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE &&
      payload->operation !=
      WYREBOX_DAEMON_AUDIT_OPERATION_WIRELOG_PREDICATE_QUERY &&
      payload->final_journal_sequence == 0) {
    g_set_error (error,
        G_IO_ERROR,
        code, "DaemonAuditRecorded final journal sequence is required");
    return FALSE;
  }

  return TRUE;
}

static gboolean
checked_add_string (gsize *total, const char *value, GError **error)
{
  gsize len = 0;

  if (!checked_add_size (total, sizeof (guint32), error))
    return FALSE;

  if (value == NULL)
    return TRUE;

  len = strlen (value);
  if (len >= WYREBOX_DAEMON_AUDIT_PAYLOAD_NULL_STRING) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "DaemonAuditRecorded payload string is too large");
    return FALSE;
  }

  return checked_add_size (total, len, error);
}

static void
write_nullable_string (guint8 **cursor, const char *value)
{
  gsize len = 0;

  if (value == NULL) {
    write_u32_le (*cursor, WYREBOX_DAEMON_AUDIT_PAYLOAD_NULL_STRING);
    *cursor += sizeof (guint32);
    return;
  }

  len = strlen (value);
  g_assert (len < WYREBOX_DAEMON_AUDIT_PAYLOAD_NULL_STRING);
  write_u32_le (*cursor, (guint32) len);
  *cursor += sizeof (guint32);
  memcpy (*cursor, value, len);
  *cursor += len;
}

static gboolean
read_nullable_string (const guint8 *data,
    gsize size, gsize *offset, char **out_value, GError **error)
{
  guint32 len = 0;

  g_assert (out_value != NULL);
  *out_value = NULL;

  if (size - *offset < sizeof (guint32)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "DaemonAuditRecorded payload is truncated");
    return FALSE;
  }

  len = read_u32_le (data + *offset);
  *offset += sizeof (guint32);
  if (len == WYREBOX_DAEMON_AUDIT_PAYLOAD_NULL_STRING)
    return TRUE;

  if ((gsize) len > size - *offset) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "DaemonAuditRecorded payload is truncated");
    return FALSE;
  }

  if (memchr (data + *offset, '\0', len) != NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DaemonAuditRecorded payload string contains embedded NUL");
    return FALSE;
  }

  *out_value = g_strndup ((const char *) data + *offset, len);
  *offset += len;
  return TRUE;
}

static void
take_decoded_payload (WyreboxDaemonAuditPayload *out_payload,
    WyreboxDaemonAuditPayload *decoded)
{
  *out_payload = *decoded;
  memset (decoded, 0, sizeof (*decoded));
}

static gboolean
payload_uses_v2 (const WyreboxDaemonAuditPayload *payload)
{
  return payload->operation ==
      WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE ||
      payload->operation ==
      WYREBOX_DAEMON_AUDIT_OPERATION_WIRELOG_PREDICATE_QUERY;
}

void
wyrebox_daemon_audit_payload_clear (WyreboxDaemonAuditPayload *payload)
{
  if (payload == NULL)
    return;

  g_clear_pointer (&payload->request_id, g_free);
  g_clear_pointer (&payload->correlation_id, g_free);
  g_clear_pointer (&payload->caller_identity, g_free);
  g_clear_pointer (&payload->account_identity, g_free);
  g_clear_pointer (&payload->tool_identity, g_free);
  g_clear_pointer (&payload->scope_id, g_free);
  g_clear_pointer (&payload->predicate_id, g_free);
  g_clear_pointer (&payload->query_id, g_free);
  g_clear_pointer (&payload->template_id, g_free);
  g_clear_pointer (&payload->error_domain, g_free);
  g_clear_pointer (&payload->error_class, g_free);
  g_clear_pointer (&payload->error_message, g_free);
  payload->operation = 0;
  payload->outcome = 0;
  payload->mutation_count = 0;
  payload->final_journal_offset = 0;
  payload->final_journal_sequence = 0;
  payload->error_code = 0;
}

GBytes *
wyrebox_daemon_audit_payload_encode (const WyreboxDaemonAuditPayload *payload,
    GError **error)
{
  g_autofree guint8 *data = NULL;
  guint8 *cursor = NULL;
  gboolean use_v2 = FALSE;
  gsize payload_len = 0;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!validate_payload (payload, G_IO_ERROR_INVALID_ARGUMENT, error))
    return NULL;

  use_v2 = payload_uses_v2 (payload);
  payload_len = use_v2 ? WYREBOX_DAEMON_AUDIT_PAYLOAD_HEADER_SIZE_V2 :
      WYREBOX_DAEMON_AUDIT_PAYLOAD_HEADER_SIZE_V1;

  if (!checked_add_string (&payload_len, payload->request_id, error) ||
      !checked_add_string (&payload_len, payload->correlation_id, error) ||
      !checked_add_string (&payload_len, payload->caller_identity, error) ||
      !checked_add_string (&payload_len, payload->account_identity, error) ||
      !checked_add_string (&payload_len, payload->tool_identity, error) ||
      !checked_add_string (&payload_len, payload->scope_id, error) ||
      !checked_add_string (&payload_len, payload->predicate_id, error))
    return NULL;

  if (use_v2 &&
      (!checked_add_string (&payload_len, payload->query_id, error) ||
          !checked_add_string (&payload_len, payload->template_id, error) ||
          !checked_add_string (&payload_len, payload->error_domain, error) ||
          !checked_add_string (&payload_len, payload->error_class, error) ||
          !checked_add_string (&payload_len, payload->error_message, error)))
    return NULL;

  data = g_malloc0 (payload_len);
  memcpy (data,
      use_v2 ? WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_V2 :
      WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_V1,
      WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_LEN);
  cursor = data + WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_LEN;
  *cursor++ = (guint8) payload->operation;
  *cursor++ = (guint8) payload->outcome;
  write_u64_le (cursor, payload->mutation_count);
  cursor += sizeof (guint64);
  write_u64_le (cursor, payload->final_journal_offset);
  cursor += sizeof (guint64);
  write_u64_le (cursor, payload->final_journal_sequence);
  cursor += sizeof (guint64);
  if (use_v2) {
    write_u64_le (cursor, (guint64) (gint64) payload->error_code);
    cursor += sizeof (guint64);
  }
  write_nullable_string (&cursor, payload->request_id);
  write_nullable_string (&cursor, payload->correlation_id);
  write_nullable_string (&cursor, payload->caller_identity);
  write_nullable_string (&cursor, payload->account_identity);
  write_nullable_string (&cursor, payload->tool_identity);
  write_nullable_string (&cursor, payload->scope_id);
  write_nullable_string (&cursor, payload->predicate_id);
  if (use_v2) {
    write_nullable_string (&cursor, payload->query_id);
    write_nullable_string (&cursor, payload->template_id);
    write_nullable_string (&cursor, payload->error_domain);
    write_nullable_string (&cursor, payload->error_class);
    write_nullable_string (&cursor, payload->error_message);
  }
  g_assert ((gsize) (cursor - data) == payload_len);

  return g_bytes_new_take (g_steal_pointer (&data), payload_len);
}

gboolean
wyrebox_daemon_audit_payload_decode (GBytes *bytes,
    WyreboxDaemonAuditPayload *out_payload, GError **error)
{
  g_auto (WyreboxDaemonAuditPayload) decoded = { 0 };
  const guint8 *data = NULL;
  gsize size = 0;
  gsize header_size = 0;
  gsize offset = 0;
  gboolean is_v2 = FALSE;

  g_return_val_if_fail (bytes != NULL, FALSE);
  g_return_val_if_fail (out_payload != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  data = g_bytes_get_data (bytes, &size);

  if (size >= WYREBOX_DAEMON_AUDIT_PAYLOAD_HEADER_SIZE_V1 &&
      memcmp (data, WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_V1,
          WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_LEN) == 0) {
    header_size = WYREBOX_DAEMON_AUDIT_PAYLOAD_HEADER_SIZE_V1;
  } else if (size >= WYREBOX_DAEMON_AUDIT_PAYLOAD_HEADER_SIZE_V2 &&
      memcmp (data, WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_V2,
          WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_LEN) == 0) {
    header_size = WYREBOX_DAEMON_AUDIT_PAYLOAD_HEADER_SIZE_V2;
    is_v2 = TRUE;
  } else if (size < WYREBOX_DAEMON_AUDIT_PAYLOAD_HEADER_SIZE_V1) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "DaemonAuditRecorded payload is truncated");
    return FALSE;
  } else {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DaemonAuditRecorded payload magic is unsupported");
    return FALSE;
  }

  offset = header_size;
  decoded.operation = (WyreboxDaemonAuditOperation)
      data[WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_LEN];
  decoded.outcome = (WyreboxDaemonAuditOutcome)
      data[WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_LEN + 1];
  decoded.mutation_count =
      read_u64_le (data + WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_LEN + 2);
  decoded.final_journal_offset =
      read_u64_le (data + WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_LEN + 10);
  decoded.final_journal_sequence =
      read_u64_le (data + WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_LEN + 18);
  if (is_v2) {
    decoded.error_code = (gint) (gint64)
        read_u64_le (data + WYREBOX_DAEMON_AUDIT_PAYLOAD_MAGIC_LEN + 26);
  }

  if (!read_nullable_string (data, size, &offset, &decoded.request_id, error) ||
      !read_nullable_string (data, size, &offset, &decoded.correlation_id,
          error) ||
      !read_nullable_string (data, size, &offset, &decoded.caller_identity,
          error) ||
      !read_nullable_string (data, size, &offset, &decoded.account_identity,
          error) ||
      !read_nullable_string (data, size, &offset, &decoded.tool_identity,
          error) ||
      !read_nullable_string (data, size, &offset, &decoded.scope_id, error) ||
      !read_nullable_string (data, size, &offset, &decoded.predicate_id, error))
    return FALSE;

  if (is_v2 &&
      (!read_nullable_string (data, size, &offset, &decoded.query_id, error) ||
          !read_nullable_string (data, size, &offset, &decoded.template_id,
              error) ||
          !read_nullable_string (data, size, &offset, &decoded.error_domain,
              error) ||
          !read_nullable_string (data, size, &offset, &decoded.error_class,
              error) ||
          !read_nullable_string (data, size, &offset, &decoded.error_message,
              error)))
    return FALSE;

  if (offset != size) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DaemonAuditRecorded payload has trailing bytes");
    return FALSE;
  }

  if (!validate_payload (&decoded, G_IO_ERROR_INVALID_DATA, error))
    return FALSE;

  wyrebox_daemon_audit_payload_clear (out_payload);
  take_decoded_payload (out_payload, &decoded);
  return TRUE;
}
