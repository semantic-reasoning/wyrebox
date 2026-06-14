#include "wyrebox-daemon-fact-mutation-service.h"

#include "wyrebox-daemon-client-identity.h"
#include "wyrebox-daemon-audit-payload.h"
#include "wyrebox-derived-view-materializer-wirelog.h"
#include "wyrebox-fact-journal-snapshot.h"

#include <gio/gio.h>

static const char *configured_derived_view_id = "view-projects";
static const char *configured_derived_view_imap_name = "Projects";
static const char *configured_derived_view_definition_ref = "wirelog:projects";
static const char *configured_derived_view_relation_name =
    "show_in_virtual_folder";
static const char *configured_derived_view_rules_source =
    ".decl project_keyword(message_id: symbol, view_id: symbol)\n"
    ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n"
    "show_in_virtual_folder(view_id, message_id) :- "
    "project_keyword(message_id, view_id).\n";

struct _WyreboxDaemonFactMutationService
{
  GObject parent_instance;

  WyreboxJournalWriter *journal_writer;
  char *derived_view_journal_root_dir;
  char *derived_view_catalog_path;
};

G_DEFINE_TYPE (WyreboxDaemonFactMutationService,
    wyrebox_daemon_fact_mutation_service, G_TYPE_OBJECT);

static gboolean
validate_request_id (const char *request_id, GError **error)
{
  if (request_id == NULL || *request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact mutation request_id is required");
    return FALSE;
  }

  return TRUE;
}

static gboolean
authorize_fact_mutation_identity (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFactMutationRequest *request, GError **error)
{
  WyreboxDaemonClientIdentityClass identity_class =
      wyrebox_daemon_client_identity_classify_request (identity);

  if (!wyrebox_daemon_client_identity_can_mutate_facts (identity_class)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized to mutate facts");
    return FALSE;
  }

  if (identity->account_identity == NULL ||
      g_strcmp0 (identity->account_identity, request->scope_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized for fact mutation account scope");
    return FALSE;
  }

  return TRUE;
}

static gboolean
authorize_fact_batch_import_identity (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonFactBatchImportRequest *request,
    GError **error)
{
  WyreboxDaemonClientIdentityClass identity_class =
      wyrebox_daemon_client_identity_classify_request (identity);
  const char *scope_id = NULL;

  if (!wyrebox_daemon_client_identity_can_mutate_facts (identity_class)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized to import fact batches");
    return FALSE;
  }

  scope_id = wyrebox_daemon_fact_batch_import_request_get_scope_id (request);
  if (identity->account_identity == NULL ||
      g_strcmp0 (identity->account_identity, scope_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized for fact batch import account scope");
    return FALSE;
  }

  return TRUE;
}

static void
wyrebox_daemon_fact_mutation_service_finalize (GObject *object)
{
  WyreboxDaemonFactMutationService *self =
      WYREBOX_DAEMON_FACT_MUTATION_SERVICE (object);

  g_clear_pointer (&self->derived_view_journal_root_dir, g_free);
  g_clear_pointer (&self->derived_view_catalog_path, g_free);
  g_clear_object (&self->journal_writer);

  G_OBJECT_CLASS (wyrebox_daemon_fact_mutation_service_parent_class)->finalize
      (object);
}

static void
    wyrebox_daemon_fact_mutation_service_class_init
    (WyreboxDaemonFactMutationServiceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_fact_mutation_service_finalize;
}

static void
wyrebox_daemon_fact_mutation_service_init (WyreboxDaemonFactMutationService
    *self)
{
}

WyreboxDaemonFactMutationService *
wyrebox_daemon_fact_mutation_service_new (WyreboxJournalWriter *journal_writer)
{
  g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (journal_writer), NULL);

  WyreboxDaemonFactMutationService *self =
      g_object_new (WYREBOX_TYPE_DAEMON_FACT_MUTATION_SERVICE, NULL);

  self->journal_writer = g_object_ref (journal_writer);

  return self;
}

gboolean
    wyrebox_daemon_fact_mutation_service_configure_wirelog_derived_view
    (WyreboxDaemonFactMutationService * self, const char *journal_root_dir,
    const char *catalog_path, GError ** error)
{
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (journal_root_dir == NULL || *journal_root_dir == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view materialization journal root is required");
    return FALSE;
  }

  if (catalog_path == NULL || *catalog_path == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view materialization catalog path is required");
    return FALSE;
  }

  materializer = wyrebox_derived_view_materializer_new_duckdb (catalog_path,
      error);
  if (materializer == NULL)
    return FALSE;

  g_free (self->derived_view_journal_root_dir);
  self->derived_view_journal_root_dir = g_strdup (journal_root_dir);
  g_free (self->derived_view_catalog_path);
  self->derived_view_catalog_path = g_strdup (catalog_path);

  return TRUE;
}

static gboolean
append_audit_record (WyreboxDaemonFactMutationService *self,
    WyreboxDaemonAuditOperation operation,
    const WyreboxDaemonRequestIdentity *identity,
    const char *scope_id,
    guint64 mutation_count,
    const char *predicate_id,
    guint64 final_journal_offset,
    guint64 final_journal_sequence, GError **error)
{
  g_autoptr (GBytes) payload_bytes = NULL;
  guint64 audit_offset = 0;
  guint64 audit_sequence = 0;
  WyreboxDaemonAuditPayload payload = {
    .operation = operation,
    .outcome = WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS,
    .request_id = (char *) identity->request_id,
    .correlation_id = (char *) identity->correlation_id,
    .caller_identity = (char *) identity->caller_identity,
    .account_identity = (char *) identity->account_identity,
    .tool_identity = (char *) identity->tool_identity,
    .scope_id = (char *) scope_id,
    .mutation_count = mutation_count,
    .predicate_id = (char *) predicate_id,
    .final_journal_offset = final_journal_offset,
    .final_journal_sequence = final_journal_sequence,
  };

  payload_bytes = wyrebox_daemon_audit_payload_encode (&payload, error);
  if (payload_bytes == NULL)
    return FALSE;

  return wyrebox_journal_writer_append (self->journal_writer,
      WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED, payload_bytes,
      &audit_offset, &audit_sequence, error);
}

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

static GPtrArray *
filter_facts_for_scope (GPtrArray *facts, const char *scope_id, GError **error)
{
  g_autofree char *expected_source = NULL;
  g_autoptr (GPtrArray) scoped_facts = NULL;

  expected_source = g_strdup_printf ("fact-mutation:%s", scope_id);
  scoped_facts = fact_record_array_new ();

  for (guint i = 0; i < facts->len; i++) {
    const WyreboxFactRecord *record = g_ptr_array_index (facts, i);
    WyreboxFactRecord *copy = NULL;

    if (record == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA, "fact snapshot contains NULL record");
      return NULL;
    }

    if (g_strcmp0 (record->source, expected_source) != 0)
      continue;

    copy = g_new0 (WyreboxFactRecord, 1);
    if (!wyrebox_fact_record_init (copy,
            record->predicate,
            (const char *const *) record->args,
            record->source, record->confidence_ppm,
            record->created_at_unix_us, error)) {
      fact_record_ptr_free (copy);
      return NULL;
    }

    g_ptr_array_add (scoped_facts, copy);
  }

  return g_steal_pointer (&scoped_facts);
}

static gboolean
materialize_configured_derived_view (WyreboxDaemonFactMutationService *self,
    const char *scope_id, GError **error)
{
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;
  g_autoptr (GPtrArray) facts = NULL;
  g_autoptr (GPtrArray) scoped_facts = NULL;
  g_autoptr (GPtrArray) changes = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->derived_view_catalog_path == NULL)
    return TRUE;

  materializer = wyrebox_derived_view_materializer_new_duckdb
      (self->derived_view_catalog_path, error);
  if (materializer == NULL)
    return FALSE;

  facts = wyrebox_fact_journal_snapshot_load_active
      (self->derived_view_journal_root_dir, error);
  if (facts == NULL)
    return FALSE;

  scoped_facts = filter_facts_for_scope (facts, scope_id, error);
  if (scoped_facts == NULL)
    return FALSE;

  if (!wyrebox_derived_view_materializer_refresh_from_rules_and_facts_with_changes (materializer, scope_id, configured_derived_view_id, configured_derived_view_imap_name, configured_derived_view_definition_ref, (guint64) g_get_real_time (), configured_derived_view_rules_source, scoped_facts, configured_derived_view_relation_name, &changes, error))
    return FALSE;

  return wyrebox_derived_view_membership_changes_append_journal (changes,
      self->journal_writer, error);
}

static void
log_materialization_failure_if_needed (WyreboxDaemonFactMutationService *self,
    const char *scope_id, const char *operation_name)
{
  g_autoptr (GError) materialize_error = NULL;

  if (self->derived_view_catalog_path == NULL)
    return;

  if (!materialize_configured_derived_view (self, scope_id, &materialize_error)) {
    g_debug ("%s materialization failed after durable commit: %s",
        operation_name, materialize_error->message);
  }
}

static gboolean
handle_authorized_fact_mutation (WyreboxDaemonFactMutationService
    *self, const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFactMutationRequest *request,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  guint64 journal_offset = 0;
  guint64 journal_sequence = 0;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (self), FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_request_id (identity->request_id, error))
    return FALSE;

  if (!wyrebox_daemon_fact_mutation_request_append_journal (request,
          self->journal_writer, &journal_offset, &journal_sequence, error))
    return FALSE;

  if (!append_audit_record (self,
          WYREBOX_DAEMON_AUDIT_OPERATION_SINGLE_FACT_MUTATION, identity,
          request->scope_id, 1, request->predicate_id, journal_offset,
          journal_sequence, error))
    return FALSE;

  log_materialization_failure_if_needed (self, request->scope_id,
      "fact mutation");

  return wyrebox_daemon_response_frame_init_fact_mutation_success (out_frame,
      identity->request_id, identity->correlation_id, request, journal_offset,
      journal_sequence, error);
}

static gboolean
handle_authorized_fact_batch_import (WyreboxDaemonFactMutationService
    *self, const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFactBatchImportRequest *request,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  guint n_entries = 0;
  guint64 journal_offset = 0;
  guint64 journal_sequence = 0;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (self), FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_request_id (identity->request_id, error))
    return FALSE;

  if (!wyrebox_daemon_fact_batch_import_request_validate (request, error))
    return FALSE;

  n_entries = wyrebox_daemon_fact_batch_import_request_get_n_entries (request);
  for (guint i = 0; i < n_entries; i++) {
    const WyreboxDaemonFactMutationRequest *entry =
        wyrebox_daemon_fact_batch_import_request_get_entry (request, i);

    if (!wyrebox_daemon_fact_mutation_request_append_journal (entry,
            self->journal_writer, &journal_offset, &journal_sequence, error))
      return FALSE;
  }

  if (!append_audit_record (self,
          WYREBOX_DAEMON_AUDIT_OPERATION_FACT_BATCH_IMPORT, identity,
          wyrebox_daemon_fact_batch_import_request_get_scope_id (request),
          n_entries, NULL, journal_offset, journal_sequence, error))
    return FALSE;

  log_materialization_failure_if_needed (self,
      wyrebox_daemon_fact_batch_import_request_get_scope_id (request),
      "fact batch import");

  return wyrebox_daemon_response_frame_init_fact_batch_import_success
      (out_frame, identity->request_id, identity->correlation_id, request,
      journal_offset, journal_sequence, error);
}

gboolean
    wyrebox_daemon_fact_mutation_service_handle_identity
    (WyreboxDaemonFactMutationService * self,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonFactMutationRequest * request,
    WyreboxDaemonResponseFrame * out_frame, GError ** error)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (self), FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!authorize_fact_mutation_identity (identity, request, error))
    return FALSE;

  return handle_authorized_fact_mutation (self, identity, request, out_frame,
      error);
}

gboolean
    wyrebox_daemon_fact_mutation_service_handle_batch_identity
    (WyreboxDaemonFactMutationService * self,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonFactBatchImportRequest * request,
    WyreboxDaemonResponseFrame * out_frame, GError ** error)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (self), FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_daemon_fact_batch_import_request_validate (request, error))
    return FALSE;

  if (!authorize_fact_batch_import_identity (identity, request, error))
    return FALSE;

  return handle_authorized_fact_batch_import (self, identity, request,
      out_frame, error);
}
