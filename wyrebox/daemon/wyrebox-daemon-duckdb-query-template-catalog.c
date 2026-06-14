#include "wyrebox-daemon-duckdb-query-template-catalog.h"

#include <gio/gio.h>

#include <errno.h>

static const char *const mailbox_uid_map_parameters[] = { "mailbox_id", NULL };
static const char *const derived_view_uid_map_parameters[] =
    { "view_id", NULL };
static const char *const message_by_id_parameters[] = { "message_id", NULL };
static const char *const messages_by_from_addr_parameters[] =
    { "from_addr", NULL };
static const char *const messages_by_sender_domain_parameters[] =
    { "sender_domain", NULL };
static const char *const messages_by_subject_parameters[] = { "subject", NULL };
static const char *const messages_by_date_range_parameters[] =
    { "start_unix_us", "end_unix_us", NULL };

static const WyreboxDaemonDuckDBQueryTemplateDescriptor catalog[] = {
  {
        "mailbox.uid_map.v1",
        "mailbox uid map",
        "account_id",
        "stream-chunk.duckdb-template.uid-map.v1",
        1,
      mailbox_uid_map_parameters},
  {
        "derived_view.uid_map.v1",
        "derived view uid map",
        "account_id",
        "stream-chunk.duckdb-template.derived-view-uid-map.v1",
        1,
      derived_view_uid_map_parameters},
  {
        "message.by_id.v1",
        "message by id",
        "account_id",
        "stream-chunk.duckdb-template.message-by-id.v1",
        1,
      message_by_id_parameters},
  {
        "messages.by_from_addr.v1",
        "messages by from address",
        "account_id",
        "stream-chunk.duckdb-template.messages-by-from-addr.v1",
        1,
      messages_by_from_addr_parameters},
  {
        "messages.by_sender_domain.v1",
        "messages by sender domain",
        "account_id",
        "stream-chunk.duckdb-template.messages-by-sender-domain.v1",
        1,
      messages_by_sender_domain_parameters},
  {
        "messages.by_subject.v1",
        "messages by subject",
        "account_id",
        "stream-chunk.duckdb-template.messages-by-subject.v1",
        1,
      messages_by_subject_parameters},
  {
        "messages.by_date_range.v1",
        "messages by date range",
        "account_id",
        "stream-chunk.duckdb-template.messages-by-date-range.v1",
        2,
      messages_by_date_range_parameters},
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
    validate_messages_by_date_range_parameters
    (const WyreboxDaemonDuckDBQueryTemplateRequest * request, GError ** error)
{
  return validate_signed_integer_text (request->parameters[0],
      "start_unix_us", error)
      && validate_signed_integer_text (request->parameters[1], "end_unix_us",
      error);
}

static gboolean
    validate_template_parameters
    (const WyreboxDaemonDuckDBQueryTemplateDescriptor * descriptor,
    const WyreboxDaemonDuckDBQueryTemplateRequest * request, GError ** error)
{
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
