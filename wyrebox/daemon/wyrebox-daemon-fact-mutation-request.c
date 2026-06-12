#include "wyrebox-daemon-fact-mutation-request.h"

#include <gio/gio.h>
#include <string.h>

#define WYREBOX_FACT_MUTATION_PAYLOAD_MAGIC "WYREFMP1"
#define WYREBOX_FACT_MUTATION_PAYLOAD_MAGIC_LEN 8
#define WYREBOX_FACT_MUTATION_PAYLOAD_HEADER_SIZE 9

static inline void
write_u32_le (guint8 *dst, guint32 value)
{
  dst[0] = (guint8) ((value >> 0) & 0xFF);
  dst[1] = (guint8) ((value >> 8) & 0xFF);
  dst[2] = (guint8) ((value >> 16) & 0xFF);
  dst[3] = (guint8) ((value >> 24) & 0xFF);
}

static inline guint32
read_u32_le (const guint8 *buffer)
{
  return (guint32) buffer[0] |
      ((guint32) buffer[1] << 8) |
      ((guint32) buffer[2] << 16) | ((guint32) buffer[3] << 24);
}

static gboolean
is_supported_mutation (WyreboxDaemonFactMutationKind mutation)
{
  return wyrebox_daemon_fact_mutation_kind_to_wire_name (mutation) != NULL;
}

static gboolean
is_predicate_start (char value)
{
  return g_ascii_islower (value) || value == '_';
}

static gboolean
is_predicate_char (char value)
{
  return g_ascii_isalnum (value) || value == '_';
}

static gboolean
validate_predicate_id (const char *predicate_id, GError **error)
{
  if (predicate_id == NULL || *predicate_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact mutation predicate_id is required");
    return FALSE;
  }

  if (!is_predicate_start (predicate_id[0])) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation predicate_id must start with lowercase ASCII or underscore");
    return FALSE;
  }

  for (const char *cursor = predicate_id + 1; *cursor != '\0'; cursor++) {
    if (!is_predicate_char (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "fact mutation predicate_id contains an unsupported character");
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_required_text (const char *value,
    const char *field_name, GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "fact mutation %s must not contain control characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_arguments (const char *const *arguments, GError **error)
{
  if (arguments == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation arguments vector is required");
    return FALSE;
  }

  for (guint index = 0; arguments[index] != NULL; index++) {
    if (arguments[index][0] == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "fact mutation arguments must not contain empty values");
      return FALSE;
    }

    for (const char *cursor = arguments[index]; *cursor != '\0'; cursor++) {
      if (g_ascii_iscntrl (*cursor)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "fact mutation arguments must not contain control characters");
        return FALSE;
      }
    }
  }

  return TRUE;
}

static gboolean
request_is_initialized (const WyreboxDaemonFactMutationRequest *request)
{
  return request != NULL &&
      request->predicate_id != NULL &&
      request->scope_id != NULL && request->arguments != NULL;
}

static gboolean
checked_add_size (gsize *total, gsize value, GError **error)
{
  if (value > G_MAXSIZE - *total) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "FactMutation payload length overflows addressable memory");
    return FALSE;
  }

  *total += value;
  return TRUE;
}

static gboolean
checked_add_encoded_string_len (gsize *total, const char *value, GError **error)
{
  gsize value_len = 0;

  if (!checked_add_size (total, sizeof (guint32), error))
    return FALSE;

  value_len = strlen (value);
  if (value_len > G_MAXUINT32) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "FactMutation string is too large");
    return FALSE;
  }

  return checked_add_size (total, value_len, error);
}

static guint
count_arguments (char **arguments)
{
  guint count = 0;

  while (arguments[count] != NULL)
    count++;

  return count;
}

static void
write_string (guint8 **cursor, const char *value)
{
  gsize value_len = strlen (value);

  g_assert (value_len <= G_MAXUINT32);

  write_u32_le (*cursor, (guint32) value_len);
  *cursor += sizeof (guint32);
  memcpy (*cursor, value, value_len);
  *cursor += value_len;
}

static gboolean
read_string (const guint8 *data,
    gsize size, gsize *offset, char **out_value, GError **error)
{
  guint32 value_len = 0;

  g_assert (out_value != NULL);
  *out_value = NULL;

  if (size - *offset < sizeof (guint32)) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "FactMutation payload is truncated");
    return FALSE;
  }

  value_len = read_u32_le (data + *offset);
  *offset += sizeof (guint32);

  if ((gsize) value_len > size - *offset) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "FactMutation payload is truncated");
    return FALSE;
  }

  if (memchr (data + *offset, '\0', value_len) != NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "FactMutation string contains embedded NUL");
    return FALSE;
  }

  *out_value = g_strndup ((const char *) data + *offset, value_len);
  *offset += value_len;

  return TRUE;
}

void
wyrebox_daemon_fact_mutation_request_clear (WyreboxDaemonFactMutationRequest
    *request)
{
  if (request == NULL)
    return;

  request->mutation = WYREBOX_DAEMON_FACT_MUTATION_INSERT;
  g_clear_pointer (&request->predicate_id, g_free);
  g_clear_pointer (&request->scope_id, g_free);
  g_clear_pointer (&request->arguments, g_strfreev);
}

const char *
wyrebox_daemon_fact_mutation_kind_to_wire_name (WyreboxDaemonFactMutationKind
    mutation)
{
  switch (mutation) {
    case WYREBOX_DAEMON_FACT_MUTATION_INSERT:
      return "insert";
    case WYREBOX_DAEMON_FACT_MUTATION_RETRACT:
      return "retract";
    default:
      return NULL;
  }
}

gboolean
wyrebox_daemon_fact_mutation_kind_from_wire_name (const char *wire_name,
    WyreboxDaemonFactMutationKind *mutation, GError **error)
{
  g_return_val_if_fail (mutation != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (g_strcmp0 (wire_name, "insert") == 0) {
    *mutation = WYREBOX_DAEMON_FACT_MUTATION_INSERT;
    return TRUE;
  }

  if (g_strcmp0 (wire_name, "retract") == 0) {
    *mutation = WYREBOX_DAEMON_FACT_MUTATION_RETRACT;
    return TRUE;
  }

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_ARGUMENT, "unsupported fact mutation wire name");
  return FALSE;
}

gboolean
wyrebox_daemon_fact_mutation_to_event (WyreboxDaemonFactMutationKind mutation,
    WyreboxJournalEventType *event_type, GError **error)
{
  g_return_val_if_fail (event_type != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  switch (mutation) {
    case WYREBOX_DAEMON_FACT_MUTATION_INSERT:
      *event_type = WYREBOX_JOURNAL_EVENT_FACT_INSERTED;
      return TRUE;
    case WYREBOX_DAEMON_FACT_MUTATION_RETRACT:
      *event_type = WYREBOX_JOURNAL_EVENT_FACT_RETRACTED;
      return TRUE;
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "unsupported fact mutation journal event type");
      return FALSE;
  }
}

gboolean
wyrebox_daemon_fact_mutation_request_get_event (const
    WyreboxDaemonFactMutationRequest *request,
    WyreboxJournalEventType *event_type, GError **error)
{
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (event_type != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!request_is_initialized (request)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation request is not initialized");
    return FALSE;
  }

  return wyrebox_daemon_fact_mutation_to_event (request->mutation, event_type,
      error);
}

GBytes *
wyrebox_daemon_fact_mutation_request_encode (const
    WyreboxDaemonFactMutationRequest *request, GError **error)
{
  g_autofree guint8 *buffer = NULL;
  guint argument_count = 0;
  guint8 *cursor = NULL;
  gsize payload_size = WYREBOX_FACT_MUTATION_PAYLOAD_HEADER_SIZE;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!request_is_initialized (request)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation request is not initialized");
    return NULL;
  }

  if (!is_supported_mutation (request->mutation)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "unsupported fact mutation kind");
    return NULL;
  }

  if (!validate_predicate_id (request->predicate_id, error))
    return NULL;

  if (!validate_required_text (request->scope_id, "scope_id", error))
    return NULL;

  if (!validate_arguments ((const char *const *) request->arguments, error))
    return NULL;

  argument_count = count_arguments (request->arguments);
  if (!checked_add_encoded_string_len (&payload_size, request->predicate_id,
          error))
    return NULL;
  if (!checked_add_encoded_string_len (&payload_size, request->scope_id, error))
    return NULL;
  if (!checked_add_size (&payload_size, sizeof (guint32), error))
    return NULL;

  for (guint index = 0; index < argument_count; index++) {
    if (!checked_add_encoded_string_len (&payload_size,
            request->arguments[index], error))
      return NULL;
  }

  buffer = g_malloc (payload_size);
  cursor = buffer;
  memcpy (cursor,
      WYREBOX_FACT_MUTATION_PAYLOAD_MAGIC,
      WYREBOX_FACT_MUTATION_PAYLOAD_MAGIC_LEN);
  cursor += WYREBOX_FACT_MUTATION_PAYLOAD_MAGIC_LEN;
  *cursor = (guint8) request->mutation;
  cursor++;
  write_string (&cursor, request->predicate_id);
  write_string (&cursor, request->scope_id);
  write_u32_le (cursor, argument_count);
  cursor += sizeof (guint32);

  for (guint index = 0; index < argument_count; index++)
    write_string (&cursor, request->arguments[index]);

  g_assert ((gsize) (cursor - buffer) == payload_size);

  return g_bytes_new_take (g_steal_pointer (&buffer), payload_size);
}

gboolean
wyrebox_daemon_fact_mutation_request_decode (GBytes *bytes,
    WyreboxDaemonFactMutationRequest *out_request, GError **error)
{
  const guint8 *data = NULL;
  gsize size = 0;
  gsize offset = 0;
  guint32 argument_count = 0;
  g_autofree char *predicate_id = NULL;
  g_autofree char *scope_id = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) decoded = { 0 };
  g_autoptr (GPtrArray) arguments = NULL;
  WyreboxDaemonFactMutationKind mutation = WYREBOX_DAEMON_FACT_MUTATION_INSERT;

  g_return_val_if_fail (bytes != NULL, FALSE);
  g_return_val_if_fail (out_request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  data = g_bytes_get_data (bytes, &size);
  if (size < WYREBOX_FACT_MUTATION_PAYLOAD_HEADER_SIZE) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "FactMutation payload is truncated");
    return FALSE;
  }

  if (memcmp (data,
          WYREBOX_FACT_MUTATION_PAYLOAD_MAGIC,
          WYREBOX_FACT_MUTATION_PAYLOAD_MAGIC_LEN) != 0) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "unsupported FactMutation payload");
    return FALSE;
  }

  offset = WYREBOX_FACT_MUTATION_PAYLOAD_MAGIC_LEN;
  mutation = (WyreboxDaemonFactMutationKind) data[offset];
  offset++;

  if (!is_supported_mutation (mutation)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "unsupported FactMutation mutation kind");
    return FALSE;
  }

  if (!read_string (data, size, &offset, &predicate_id, error))
    return FALSE;
  if (!read_string (data, size, &offset, &scope_id, error))
    return FALSE;

  if (size - offset < sizeof (guint32)) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "FactMutation payload is truncated");
    return FALSE;
  }

  argument_count = read_u32_le (data + offset);
  offset += sizeof (guint32);
  arguments = g_ptr_array_new_with_free_func (g_free);

  for (guint32 index = 0; index < argument_count; index++) {
    g_autofree char *argument = NULL;

    if (!read_string (data, size, &offset, &argument, error))
      return FALSE;

    g_ptr_array_add (arguments, g_steal_pointer (&argument));
  }

  if (offset != size) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "FactMutation payload contains trailing bytes");
    return FALSE;
  }

  g_ptr_array_add (arguments, NULL);
  if (!wyrebox_daemon_fact_mutation_request_init (&decoded,
          mutation,
          predicate_id,
          scope_id, (const char *const *) arguments->pdata, error))
    return FALSE;

  wyrebox_daemon_fact_mutation_request_clear (out_request);
  *out_request = decoded;
  memset (&decoded, 0, sizeof (decoded));

  return TRUE;
}

gboolean
wyrebox_daemon_fact_mutation_request_append_journal (const
    WyreboxDaemonFactMutationRequest *request,
    WyreboxJournalWriter *journal_writer,
    guint64 *out_offset, guint64 *out_sequence, GError **error)
{
  WyreboxJournalEventType event_type = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
  g_autoptr (GBytes) payload = NULL;

  g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (journal_writer), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_daemon_fact_mutation_request_get_event (request,
          &event_type, error))
    return FALSE;

  payload = wyrebox_daemon_fact_mutation_request_encode (request, error);
  if (payload == NULL)
    return FALSE;

  return wyrebox_journal_writer_append (journal_writer, event_type, payload,
      out_offset, out_sequence, error);
}

gboolean
wyrebox_daemon_fact_mutation_request_init (WyreboxDaemonFactMutationRequest
    *request, WyreboxDaemonFactMutationKind mutation, const char *predicate_id,
    const char *scope_id, const char *const *arguments, GError **error)
{
  g_auto (WyreboxDaemonFactMutationRequest) next = { 0 };

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!is_supported_mutation (mutation)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "unsupported fact mutation kind");
    return FALSE;
  }

  if (!validate_predicate_id (predicate_id, error))
    return FALSE;

  if (!validate_required_text (scope_id, "scope_id", error))
    return FALSE;

  if (!validate_arguments (arguments, error))
    return FALSE;

  next.mutation = mutation;
  next.predicate_id = g_strdup (predicate_id);
  next.scope_id = g_strdup (scope_id);
  next.arguments = g_strdupv ((char **) arguments);

  wyrebox_daemon_fact_mutation_request_clear (request);
  *request = next;
  memset (&next, 0, sizeof (next));

  return TRUE;
}
