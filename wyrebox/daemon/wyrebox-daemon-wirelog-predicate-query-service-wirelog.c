#include "wyrebox-daemon-wirelog-predicate-query-service.h"

#include "wyrebox-daemon-client-identity.h"
#include "wyrebox-daemon-wirelog-predicate-query-catalog.h"
#include "wyrebox-fact-journal-snapshot.h"
#include "wyrebox-wirelog-derived-membership.h"
#include "wyrebox-wirelog-program.h"

#include <gio/gio.h>

typedef struct
{
  char *rules_source;
  char *journal_root_dir;
} WirelogPredicateQueryExecutor;

static void
fact_record_ptr_free (gpointer data)
{
  WyreboxFactRecord *record = data;

  if (record == NULL)
    return;

  wyrebox_fact_record_clear (record);
  g_free (record);
}

static GPtrArray *
fact_record_array_new (void)
{
  return g_ptr_array_new_with_free_func (fact_record_ptr_free);
}

static gboolean
copy_fact_record (const WyreboxFactRecord *record,
    WyreboxFactRecord *out_record, GError **error)
{
  return wyrebox_fact_record_init (out_record, record->predicate,
      (const char *const *) record->args, record->source,
      record->confidence_ppm, record->created_at_unix_us, error);
}

static GPtrArray *
filter_facts_for_account_scope (GPtrArray *facts,
    const char *account_id, GError **error)
{
  g_autofree char *expected_source = NULL;
  g_autoptr (GPtrArray) scoped = NULL;

  expected_source = g_strdup_printf ("fact-mutation:%s", account_id);
  scoped = fact_record_array_new ();

  for (guint i = 0; i < facts->len; i++) {
    const WyreboxFactRecord *record = g_ptr_array_index (facts, i);
    WyreboxFactRecord *copy = NULL;

    if (record == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT, "fact snapshot contains NULL record");
      return NULL;
    }

    if (g_strcmp0 (record->source, expected_source) != 0)
      continue;

    copy = g_new0 (WyreboxFactRecord, 1);
    if (!copy_fact_record (record, copy, error)) {
      fact_record_ptr_free (copy);
      return NULL;
    }

    g_ptr_array_add (scoped, copy);
  }

  return g_steal_pointer (&scoped);
}

static void
csv_append_value (GString *csv, const char *value)
{
  gboolean needs_quote = FALSE;

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (*cursor == '"' || *cursor == ',' || *cursor == '\n' || *cursor == '\r') {
      needs_quote = TRUE;
      break;
    }
  }

  if (!needs_quote) {
    g_string_append (csv, value);
    return;
  }

  g_string_append_c (csv, '"');
  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (*cursor == '"')
      g_string_append_c (csv, '"');
    g_string_append_c (csv, *cursor);
  }
  g_string_append_c (csv, '"');
}

static GBytes *
wirelog_predicate_memberships_to_csv (const char *account_id,
    GPtrArray *memberships)
{
  g_autoptr (GString) csv = NULL;
  gsize csv_len = 0;
  char *csv_data = NULL;

  csv = g_string_new ("account_id,view_id,message_id\n");

  for (guint i = 0; i < memberships->len; i++) {
    const WyreboxWirelogDerivedMembership *membership =
        g_ptr_array_index (memberships, i);

    csv_append_value (csv, account_id);
    g_string_append_c (csv, ',');
    csv_append_value (csv, membership->view_id);
    g_string_append_c (csv, ',');
    csv_append_value (csv, membership->message_id);
    g_string_append_c (csv, '\n');
  }

  csv_len = csv->len;
  csv_data = g_string_free (g_steal_pointer (&csv), FALSE);
  return g_bytes_new_take (csv_data, csv_len);
}

static void
wirelog_predicate_query_executor_free (gpointer data)
{
  WirelogPredicateQueryExecutor *executor = data;

  if (executor == NULL)
    return;

  g_clear_pointer (&executor->rules_source, g_free);
  g_clear_pointer (&executor->journal_root_dir, g_free);
  g_free (executor);
}

/* *INDENT-OFF* */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (WirelogPredicateQueryExecutor,
    wirelog_predicate_query_executor_free)
/* *INDENT-ON* */

static gboolean
wirelog_predicate_query_execute (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonWirelogPredicateQueryRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk, gpointer user_data,
    GError **error)
{
  WirelogPredicateQueryExecutor *executor = user_data;
  WyreboxDaemonClientIdentityClass client_class =
      wyrebox_daemon_client_identity_classify_request (identity);
  const WyreboxDaemonWirelogPredicateQueryDescriptor *descriptor = NULL;
  g_autoptr (GPtrArray) facts = NULL;
  g_autoptr (GPtrArray) scoped_facts = NULL;
  g_autoptr (GPtrArray) memberships = NULL;
  g_autoptr (GBytes) bytes = NULL;

  if (!wyrebox_daemon_wirelog_predicate_query_catalog_validate (client_class,
          identity->account_identity, request, &descriptor, error))
    return FALSE;

  facts = wyrebox_fact_journal_snapshot_load_active (executor->journal_root_dir,
      error);
  if (facts == NULL)
    return FALSE;

  scoped_facts = filter_facts_for_account_scope (facts, request->scope_id,
      error);
  if (scoped_facts == NULL)
    return FALSE;

  memberships =
      wyrebox_wirelog_derived_membership_snapshot_from_rules_and_facts
      (executor->rules_source, scoped_facts, descriptor->relation_name, error);
  if (memberships == NULL)
    return FALSE;

  bytes = wirelog_predicate_memberships_to_csv (request->scope_id, memberships);
  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

WyreboxDaemonWirelogPredicateQueryService
    * wyrebox_daemon_wirelog_predicate_query_service_new_wirelog
    (const char *rules_source, const char *journal_root_dir, GError ** error)
{
  g_autoptr (WirelogPredicateQueryExecutor) executor = NULL;
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (rules_source == NULL || rules_source[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "wirelog predicate query rules source is required");
    return NULL;
  }

  if (journal_root_dir == NULL || journal_root_dir[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "wirelog predicate query journal root is required");
    return NULL;
  }

  program = wyrebox_wirelog_program_new_from_source (rules_source, error);
  if (program == NULL)
    return NULL;

  executor = g_new0 (WirelogPredicateQueryExecutor, 1);
  executor->rules_source = g_strdup (rules_source);
  executor->journal_root_dir = g_strdup (journal_root_dir);

  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (wirelog_predicate_query_execute,
      g_steal_pointer (&executor), wirelog_predicate_query_executor_free);
  return g_steal_pointer (&service);
}
