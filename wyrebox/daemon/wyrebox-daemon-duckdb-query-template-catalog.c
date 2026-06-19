#include "wyrebox-daemon-duckdb-query-template-catalog.h"

#include <gio/gio.h>

#include <errno.h>

static const char *const mailbox_uid_map_parameters[] = { "mailbox_id", NULL };
static const char *const derived_view_uid_map_parameters[] =
    { "view_id", NULL };
static const char *const message_by_id_parameters[] = { "message_id", NULL };
static const char *const mailbox_history_by_message_parameters[] =
    { "message_id", NULL };
static const char *const message_facts_by_message_id_parameters[] =
    { "message_id", NULL };
static const char *const facts_by_source_parameters[] = { "source", NULL };
static const char *const facts_by_fact_id_parameters[] = { "fact_id", NULL };
static const char *const facts_by_fact_id_with_provenance_parameters[] =
    { "fact_id", NULL };
static const char *const messages_by_from_addr_parameters[] =
    { "from_addr", "limit", "offset", NULL };
static const char *const messages_by_sender_domain_parameters[] =
    { "sender_domain", "limit", "offset", NULL };
static const char *const messages_by_subject_parameters[] =
    { "subject", "limit", "offset", NULL };
static const char *const messages_subject_contains_parameters[] =
    { "subject_term", "limit", "offset", NULL };
static const char *const messages_by_date_range_parameters[] =
    { "start_unix_us", "end_unix_us", "limit", "offset", NULL };
static const char *const messages_by_journal_offset_range_parameters[] =
    { "start_journal_offset", "end_journal_offset", "limit", "offset", NULL };
static const char *const storage_object_statistics_parameters[] = { NULL };

static const WyreboxDaemonDuckDBQueryTemplateResultColumnDescriptor
    mailbox_uid_map_result_schema[] = {
  {"account_id", "VARCHAR", FALSE, "Mailbox account identifier"},
  {"mailbox_id", "VARCHAR", FALSE, "Mailbox identifier"},
  {"uidvalidity", "UBIGINT", FALSE, "Mailbox UID validity"},
  {"uid", "UBIGINT", FALSE, "Mailbox UID"},
  {"message_id", "VARCHAR", FALSE, "Raw message identifier"},
  {"object_id", "VARCHAR", FALSE, "Raw message object identifier"},
};

static const WyreboxDaemonDuckDBQueryTemplateResultColumnDescriptor
    derived_view_uid_map_result_schema[] = {
  {"account_id", "VARCHAR", FALSE, "Derived view account identifier"},
  {"view_id", "VARCHAR", FALSE, "Derived view identifier"},
  {"uidvalidity", "UBIGINT", FALSE, "Derived view UID validity"},
  {"uid", "UBIGINT", FALSE, "Derived view UID"},
  {"message_id", "VARCHAR", FALSE, "Raw message identifier"},
  {"object_id", "VARCHAR", FALSE, "Raw message object identifier"},
  {"rule_version_hash", "VARCHAR", FALSE, "Rule version hash"},
};

static const WyreboxDaemonDuckDBQueryTemplateResultColumnDescriptor
    message_by_id_result_schema[] = {
  {"account_id", "VARCHAR", FALSE, "Message account identifier"},
  {"message_id", "VARCHAR", FALSE, "Raw message identifier"},
  {"object_id", "VARCHAR", FALSE, "Raw message object identifier"},
  {"message_journal_offset", "UBIGINT", FALSE,
      "Raw message journal offset"},
  {"message_journal_sequence", "UBIGINT", FALSE,
      "Raw message journal sequence"},
  {"rfc_message_id", "VARCHAR", TRUE, "RFC 5322 message-id header"},
  {"subject", "VARCHAR", TRUE, "Decoded subject header"},
  {"from_addr", "VARCHAR", TRUE, "Decoded from header"},
  {"to_addr", "VARCHAR", TRUE, "Decoded to header"},
  {"cc_addr", "VARCHAR", TRUE, "Decoded cc header"},
  {"bcc_addr", "VARCHAR", TRUE, "Decoded bcc header"},
  {"date_raw", "VARCHAR", TRUE, "Decoded date header"},
  {"header_journal_offset", "UBIGINT", TRUE,
      "Header journal offset"},
  {"header_journal_sequence", "UBIGINT", TRUE,
      "Header journal sequence"},
};

static const WyreboxDaemonDuckDBQueryTemplateResultColumnDescriptor
    mailbox_history_by_message_result_schema[] = {
  {"account_id", "VARCHAR", FALSE, "Mailbox account identifier"},
  {"mailbox_id", "VARCHAR", FALSE, "Mailbox identifier"},
  {"membership_id", "VARCHAR", FALSE, "Mailbox membership identifier"},
  {"message_id", "VARCHAR", FALSE, "Raw message identifier"},
  {"uid", "UBIGINT", FALSE, "Mailbox UID"},
  {"uidvalidity", "UBIGINT", FALSE, "Mailbox UID validity"},
  {"is_visible", "BOOLEAN", FALSE, "Mailbox visibility flag"},
  {"object_id", "VARCHAR", FALSE, "Raw message object identifier"},
  {"message_journal_offset", "UBIGINT", FALSE,
      "Raw message journal offset"},
  {"message_journal_sequence", "UBIGINT", FALSE,
      "Raw message journal sequence"},
  {"membership_journal_offset", "UBIGINT", FALSE,
      "Mailbox membership journal offset"},
  {"membership_journal_sequence", "UBIGINT", FALSE,
      "Mailbox membership journal sequence"},
};

static const WyreboxDaemonDuckDBQueryTemplateResultColumnDescriptor
    message_fact_result_schema[] = {
  {"account_id", "VARCHAR", FALSE, "Fact account identifier"},
  {"message_id", "VARCHAR", FALSE, "Raw message identifier"},
  {"object_id", "VARCHAR", FALSE, "Raw message object identifier"},
  {"fact_id", "VARCHAR", FALSE, "Fact identifier"},
  {"predicate", "VARCHAR", FALSE, "Fact predicate"},
  {"args_json", "VARCHAR", FALSE, "Fact argument JSON"},
  {"source", "VARCHAR", FALSE, "Fact source"},
  {"confidence_ppm", "UBIGINT", FALSE, "Confidence in parts per million"},
  {"created_at_unix_us", "UBIGINT", FALSE, "Creation timestamp"},
  {"retracted_at_unix_us", "UBIGINT", FALSE,
      "Retraction timestamp"},
  {"journal_offset", "UBIGINT", FALSE, "Fact journal offset"},
  {"journal_sequence", "UBIGINT", FALSE, "Fact journal sequence"},
};

static const WyreboxDaemonDuckDBQueryTemplateResultColumnDescriptor
    message_fact_with_provenance_result_schema[] = {
  {"account_id", "VARCHAR", FALSE, "Fact account identifier"},
  {"message_id", "VARCHAR", FALSE, "Raw message identifier"},
  {"object_id", "VARCHAR", FALSE, "Raw message object identifier"},
  {"fact_id", "VARCHAR", FALSE, "Fact identifier"},
  {"predicate", "VARCHAR", FALSE, "Fact predicate"},
  {"args_json", "VARCHAR", FALSE, "Fact argument JSON"},
  {"source", "VARCHAR", FALSE, "Fact source"},
  {"source_span_start", "UBIGINT", TRUE, "Source span start"},
  {"source_span_end", "UBIGINT", TRUE, "Source span end"},
  {"confidence_ppm", "UBIGINT", FALSE, "Confidence in parts per million"},
  {"created_at_unix_us", "UBIGINT", FALSE, "Creation timestamp"},
  {"retracted_at_unix_us", "UBIGINT", FALSE,
      "Retraction timestamp"},
  {"journal_offset", "UBIGINT", FALSE, "Fact journal offset"},
  {"journal_sequence", "UBIGINT", FALSE, "Fact journal sequence"},
};

static const WyreboxDaemonDuckDBQueryTemplateResultColumnDescriptor
    message_search_result_schema[] = {
  {"account_id", "VARCHAR", FALSE, "Message account identifier"},
  {"message_id", "VARCHAR", FALSE, "Raw message identifier"},
  {"object_id", "VARCHAR", FALSE, "Raw message object identifier"},
  {"message_journal_offset", "UBIGINT", FALSE,
      "Raw message journal offset"},
  {"message_journal_sequence", "UBIGINT", FALSE,
      "Raw message journal sequence"},
  {"rfc_message_id", "VARCHAR", TRUE, "RFC 5322 message-id header"},
  {"subject", "VARCHAR", TRUE, "Decoded subject header"},
  {"from_addr", "VARCHAR", TRUE, "Decoded from header"},
  {"to_addr", "VARCHAR", TRUE, "Decoded to header"},
  {"cc_addr", "VARCHAR", TRUE, "Decoded cc header"},
  {"bcc_addr", "VARCHAR", TRUE, "Decoded bcc header"},
  {"date_raw", "VARCHAR", TRUE, "Decoded date header"},
  {"header_journal_offset", "UBIGINT", TRUE,
      "Header journal offset"},
  {"header_journal_sequence", "UBIGINT", TRUE,
      "Header journal sequence"},
};

static const WyreboxDaemonDuckDBQueryTemplateResultColumnDescriptor
    messages_by_journal_offset_range_result_schema[] = {
  {"account_id", "VARCHAR", FALSE, "Message account identifier"},
  {"message_id", "VARCHAR", FALSE, "Raw message identifier"},
  {"object_id", "VARCHAR", FALSE, "Raw message object identifier"},
  {"message_journal_offset", "UBIGINT", FALSE,
      "Raw message journal offset"},
  {"message_journal_sequence", "UBIGINT", FALSE,
      "Raw message journal sequence"},
  {"rfc_message_id", "VARCHAR", TRUE, "RFC 5322 message-id header"},
  {"subject", "VARCHAR", TRUE, "Decoded subject header"},
  {"from_addr", "VARCHAR", TRUE, "Decoded from header"},
  {"to_addr", "VARCHAR", TRUE, "Decoded to header"},
  {"cc_addr", "VARCHAR", TRUE, "Decoded cc header"},
  {"bcc_addr", "VARCHAR", TRUE, "Decoded bcc header"},
  {"date_raw", "VARCHAR", TRUE, "Decoded date header"},
  {"header_journal_offset", "UBIGINT", TRUE,
      "Header journal offset"},
  {"header_journal_sequence", "UBIGINT", TRUE,
      "Header journal sequence"},
};

static const WyreboxDaemonDuckDBQueryTemplateResultColumnDescriptor
    storage_object_statistics_result_schema[] = {
  {"account_id", "VARCHAR", FALSE, "Storage account identifier"},
  {"object_count", "UBIGINT", FALSE, "Stored object count"},
  {"object_size_bytes", "UBIGINT", FALSE, "Stored object byte total"},
  {"message_count", "UBIGINT", FALSE, "Message count"},
  {"mailbox_membership_count", "UBIGINT", FALSE, "Mailbox membership count"},
  {"visible_mailbox_membership_count", "UBIGINT", FALSE,
      "Visible mailbox membership count"},
  {"derived_view_membership_count", "UBIGINT", FALSE,
      "Derived view membership count"},
  {"visible_derived_view_membership_count", "UBIGINT", FALSE,
      "Visible derived view membership count"},
};

static const WyreboxDaemonDuckDBQueryTemplateDescriptor catalog[] = {
  {
        "mailbox.uid_map.v1",
        "mailbox uid map",
        "account_id",
        "stream-chunk.duckdb-template.uid-map.v1",
        1,
        mailbox_uid_map_parameters,
        G_N_ELEMENTS (mailbox_uid_map_result_schema),
      mailbox_uid_map_result_schema},
  {
        "derived_view.uid_map.v1",
        "derived view uid map",
        "account_id",
        "stream-chunk.duckdb-template.derived-view-uid-map.v1",
        1,
        derived_view_uid_map_parameters,
        G_N_ELEMENTS (derived_view_uid_map_result_schema),
      derived_view_uid_map_result_schema},
  {
        "message.by_id.v1",
        "message by id",
        "account_id",
        "stream-chunk.duckdb-template.message-by-id.v1",
        1,
        message_by_id_parameters,
        G_N_ELEMENTS (message_by_id_result_schema),
      message_by_id_result_schema},
  {
        "mailbox.history_by_message.v1",
        "mailbox history by message",
        "account_id",
        "stream-chunk.duckdb-template.mailbox-history-by-message.v1",
        1,
        mailbox_history_by_message_parameters,
        G_N_ELEMENTS (mailbox_history_by_message_result_schema),
      mailbox_history_by_message_result_schema},
  {
        "message.facts_by_message_id.v1",
        "message facts by message id",
        "account_id",
        "stream-chunk.duckdb-template.message-facts-by-message-id.v1",
        1,
        message_facts_by_message_id_parameters,
        G_N_ELEMENTS (message_fact_result_schema),
      message_fact_result_schema},
  {
        "facts.by_source.v1",
        "facts by source",
        "account_id",
        "stream-chunk.duckdb-template.facts-by-source.v1",
        1,
        facts_by_source_parameters,
        G_N_ELEMENTS (message_fact_result_schema),
      message_fact_result_schema},
  {
        "facts.by_fact_id.v1",
        "facts by fact id",
        "account_id",
        "stream-chunk.duckdb-template.facts-by-fact-id.v1",
        1,
        facts_by_fact_id_parameters,
        G_N_ELEMENTS (message_fact_result_schema),
      message_fact_result_schema},
  {
        "facts.by_fact_id_with_provenance.v1",
        "facts by fact id with provenance",
        "account_id",
        "stream-chunk.duckdb-template.facts-by-fact-id-with-provenance.v1",
        1,
        facts_by_fact_id_with_provenance_parameters,
        G_N_ELEMENTS (message_fact_with_provenance_result_schema),
      message_fact_with_provenance_result_schema},
  {
        "messages.by_from_addr.v1",
        "messages by from address",
        "account_id",
        "stream-chunk.duckdb-template.messages-by-from-addr.v1",
        3,
        messages_by_from_addr_parameters,
        G_N_ELEMENTS (message_search_result_schema),
      message_search_result_schema},
  {
        "messages.by_sender_domain.v1",
        "messages by sender domain",
        "account_id",
        "stream-chunk.duckdb-template.messages-by-sender-domain.v1",
        3,
        messages_by_sender_domain_parameters,
        G_N_ELEMENTS (message_search_result_schema),
      message_search_result_schema},
  {
        "messages.by_subject.v1",
        "messages by subject",
        "account_id",
        "stream-chunk.duckdb-template.messages-by-subject.v1",
        3,
        messages_by_subject_parameters,
        G_N_ELEMENTS (message_search_result_schema),
      message_search_result_schema},
  {
        "messages.subject_contains.v1",
        "messages subject contains",
        "account_id",
        "stream-chunk.duckdb-template.messages-subject-contains.v1",
        3,
        messages_subject_contains_parameters,
        G_N_ELEMENTS (message_search_result_schema),
      message_search_result_schema},
  {
        "messages.by_journal_offset_range.v1",
        "messages by journal offset range",
        "account_id",
        "stream-chunk.duckdb-template.messages-by-journal-offset-range.v1",
        4,
        messages_by_journal_offset_range_parameters,
        G_N_ELEMENTS (messages_by_journal_offset_range_result_schema),
      messages_by_journal_offset_range_result_schema},
  {
        "messages.by_date_range.v1",
        "messages by date range",
        "account_id",
        "stream-chunk.duckdb-template.messages-by-date-range.v1",
        4,
        messages_by_date_range_parameters,
        G_N_ELEMENTS (message_search_result_schema),
      message_search_result_schema},
  {
        "storage.object_statistics.v1",
        "storage object statistics",
        "account_id",
        "stream-chunk.duckdb-template.storage-object-statistics.v1",
        0,
        storage_object_statistics_parameters,
        G_N_ELEMENTS (storage_object_statistics_result_schema),
      storage_object_statistics_result_schema},
};

static gboolean
has_control_character (const char *value)
{
  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor))
      return TRUE;
  }

  return FALSE;
}

static gsize
count_parameters (gchar **parameters)
{
  gsize count = 0;

  if (parameters == NULL)
    return 0;

  while (parameters[count] != NULL)
    count++;

  return count;
}

static const WyreboxDaemonDuckDBQueryTemplateDescriptor *
lookup_template (const char *template_id)
{
  for (gsize i = 0; i < G_N_ELEMENTS (catalog); i++) {
    if (g_strcmp0 (catalog[i].template_id, template_id) == 0)
      return &catalog[i];
  }

  return NULL;
}

const WyreboxDaemonDuckDBQueryTemplateDescriptor *
wyrebox_daemon_duckdb_query_template_catalog_lookup (const char *template_id)
{
  return lookup_template (template_id);
}

static gboolean
is_uid_map_template (const char *template_id)
{
  return g_strcmp0 (template_id, "mailbox.uid_map.v1") == 0
      || g_strcmp0 (template_id, "derived_view.uid_map.v1") == 0;
}

static gboolean
validate_signed_integer_text (const char *value,
    const char *parameter_name, GError **error)
{
  const char *digits = value;
  gchar *end = NULL;

  if (*digits == '-') {
    digits++;

    if (*digits == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "duckdb query template parameter '%s' must be a signed integer",
          parameter_name);
      return FALSE;
    }
  }

  if (*digits == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template parameter '%s' must be a signed integer",
        parameter_name);
    return FALSE;
  }

  for (const char *cursor = digits; *cursor != '\0'; cursor++) {
    if (!g_ascii_isdigit (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "duckdb query template parameter '%s' must be a signed integer",
          parameter_name);
      return FALSE;
    }
  }

  errno = 0;
  (void) g_ascii_strtoll (value, &end, 10);
  if (errno == ERANGE || end == NULL || *end != '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template parameter '%s' is outside the signed integer "
        "range", parameter_name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_unsigned_integer_text (const char *value,
    const char *parameter_name, guint64 *out_value, GError **error)
{
  gchar *end = NULL;
  guint64 parsed = 0;

  if (*value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template parameter '%s' must be an unsigned integer",
        parameter_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (!g_ascii_isdigit (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "duckdb query template parameter '%s' must be an unsigned integer",
          parameter_name);
      return FALSE;
    }
  }

  errno = 0;
  parsed = g_ascii_strtoull (value, &end, 10);
  if (errno == ERANGE || end == NULL || *end != '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template parameter '%s' is outside the unsigned "
        "integer range", parameter_name);
    return FALSE;
  }

  if (out_value != NULL)
    *out_value = parsed;

  return TRUE;
}

static gboolean
validate_limit_offset_parameters (const char *limit_text,
    const char *offset_text, GError **error)
{
  guint64 limit = 0;
  guint64 offset = 0;

  if (!validate_unsigned_integer_text (limit_text, "limit", &limit, error) ||
      !validate_unsigned_integer_text (offset_text, "offset", &offset, error))
    return FALSE;

  if (limit == 0 || limit > 100) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template parameter 'limit' must be between 1 and 100");
    return FALSE;
  }

  if (offset > G_MAXINT64) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template parameter 'offset' must be within the signed "
        "integer range");
    return FALSE;
  }

  return TRUE;
}

static gboolean
    validate_messages_by_date_range_parameters
    (const WyreboxDaemonDuckDBQueryTemplateRequest * request, GError ** error)
{
  return validate_signed_integer_text (request->parameters[0],
      "start_unix_us", error)
      && validate_signed_integer_text (request->parameters[1], "end_unix_us",
      error)
      && validate_limit_offset_parameters (request->parameters[2],
      request->parameters[3], error);
}

static gboolean
    validate_template_parameters
    (const WyreboxDaemonDuckDBQueryTemplateDescriptor * descriptor,
    const WyreboxDaemonDuckDBQueryTemplateRequest * request, GError ** error)
{
  if (g_strcmp0 (descriptor->template_id, "messages.by_from_addr.v1") == 0 ||
      g_strcmp0 (descriptor->template_id, "messages.by_sender_domain.v1") ==
      0 || g_strcmp0 (descriptor->template_id, "messages.by_subject.v1") == 0
      || g_strcmp0 (descriptor->template_id, "messages.subject_contains.v1")
      == 0)
    return validate_limit_offset_parameters (request->parameters[1],
        request->parameters[2], error);

  if (g_strcmp0 (descriptor->template_id, "messages.by_date_range.v1") == 0)
    return validate_messages_by_date_range_parameters (request, error);

  return TRUE;
}

gboolean
    wyrebox_daemon_duckdb_query_template_catalog_validate
    (WyreboxDaemonClientIdentityClass client_class,
    const char *caller_account_id,
    const WyreboxDaemonDuckDBQueryTemplateRequest * request,
    const WyreboxDaemonDuckDBQueryTemplateDescriptor ** out_descriptor,
    GError ** error)
{
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  gsize parameter_count = 0;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (out_descriptor != NULL)
    *out_descriptor = NULL;

  if (!wyrebox_daemon_client_identity_can_query_controlled_views
      (client_class) && client_class !=
      WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized to query duckdb query templates");
    return FALSE;
  }

  if (request == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template request is required");
    return FALSE;
  }

  if (caller_account_id == NULL || *caller_account_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller account scope is required for duckdb query template");
    return FALSE;
  }

  if (request->scope_id == NULL || *request->scope_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "duckdb query template account scope is required");
    return FALSE;
  }

  if (g_strcmp0 (caller_account_id, request->scope_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized for duckdb query template account scope");
    return FALSE;
  }

  descriptor = lookup_template (request->template_id);
  if (descriptor == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "unknown duckdb query template '%s'",
        request->template_id != NULL ? request->template_id : "(null)");
    return FALSE;
  }

  if (client_class == WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN &&
      !is_uid_map_template (descriptor->template_id)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized for duckdb query template '%s'",
        request->template_id);
    return FALSE;
  }

  parameter_count = count_parameters (request->parameters);
  if (parameter_count != descriptor->n_parameters) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template '%s' expects %" G_GSIZE_FORMAT " parameter(s)",
        descriptor->template_id, descriptor->n_parameters);
    return FALSE;
  }

  for (gsize i = 0; i < parameter_count; i++) {
    const char *parameter = request->parameters[i];

    if (parameter == NULL || *parameter == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "duckdb query template parameter is required");
      return FALSE;
    }

    if (has_control_character (parameter)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "duckdb query template parameter must not contain control "
          "characters");
      return FALSE;
    }
  }

  if (!validate_template_parameters (descriptor, request, error))
    return FALSE;

  if (out_descriptor != NULL)
    *out_descriptor = descriptor;

  return TRUE;
}
