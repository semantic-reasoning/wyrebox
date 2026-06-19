#include "wyrebox-daemon-fact-mutation-service.h"

#include "wyrebox-daemon-derived-view-catalog.h"
#include "wyrebox-daemon-client-identity.h"
#include "wyrebox-daemon-audit-payload.h"
#include "wyrebox-derived-view-materializer-wirelog.h"
#include "wyrebox-fact-journal-snapshot.h"

#include <gio/gio.h>

struct _WyreboxDaemonFactMutationService
{
  GObject parent_instance;

  WyreboxJournalWriter *journal_writer;
  char *derived_view_journal_root_dir;
  char *derived_view_catalog_path;
  WyreboxDaemonDerivedViewCatalog *derived_view_catalog;
};

static const char *configured_derived_view_id = "view-projects";
static const char *configured_derived_view_imap_name = "Projects";
static const char *configured_derived_view_definition_ref = "wirelog:projects";
static const char *configured_derived_view_relation_name =
    "show_in_virtual_folder";
static const char *configured_derived_view_rules_source =
    ".decl project_keyword(message_id: symbol, view_id: symbol)\n"
    ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n"
    "show_in_virtual_folder(\"view-projects\", message_id) :- "
    "project_keyword(message_id, \"view-projects\").\n";

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
  g_clear_object (&self->derived_view_catalog);
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
  self->derived_view_catalog = wyrebox_daemon_derived_view_catalog_new ();
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

static gboolean
    register_default_derived_view_if_needed
    (WyreboxDaemonFactMutationService * self, GError ** error)
{
  WyreboxDaemonDerivedViewDefinition definition = {
    .view_id = (gchar *) configured_derived_view_id,
    .imap_name = (gchar *) configured_derived_view_imap_name,
    .definition_ref = (gchar *) configured_derived_view_definition_ref,
    .rules_source = (gchar *) configured_derived_view_rules_source,
    .relation_name = (gchar *) configured_derived_view_relation_name,
  };
  guint n_definitions = 0;

  n_definitions = wyrebox_daemon_derived_view_catalog_get_n_definitions
      (self->derived_view_catalog);
  for (guint i = 0; i < n_definitions; i++) {
    const WyreboxDaemonDerivedViewDefinition *current = NULL;

    current = wyrebox_daemon_derived_view_catalog_get_definition
        (self->derived_view_catalog, i);
    if (current != NULL &&
        g_strcmp0 (current->view_id, configured_derived_view_id) == 0) {
      if (g_strcmp0 (current->imap_name,
              configured_derived_view_imap_name) == 0 &&
          g_strcmp0 (current->definition_ref,
              configured_derived_view_definition_ref) == 0 &&
          g_strcmp0 (current->rules_source,
              configured_derived_view_rules_source) == 0 &&
          g_strcmp0 (current->relation_name,
              configured_derived_view_relation_name) == 0)
        return TRUE;

      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_EXISTS,
          "default derived view '%s' is already registered with a different "
          "definition", configured_derived_view_id);
      return FALSE;
    }
  }

  return wyrebox_daemon_derived_view_catalog_register_definition
      (self->derived_view_catalog, &definition, error);
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

  if (!register_default_derived_view_if_needed (self, error))
    return FALSE;

  g_free (self->derived_view_journal_root_dir);
  self->derived_view_journal_root_dir = g_strdup (journal_root_dir);
  g_free (self->derived_view_catalog_path);
  self->derived_view_catalog_path = g_strdup (catalog_path);

  return TRUE;
}

gboolean
    wyrebox_daemon_fact_mutation_service_register_wirelog_derived_view
    (WyreboxDaemonFactMutationService * self, const char *view_id,
    const char *imap_name, const char *definition_ref,
    const char *rules_source, const char *relation_name, GError ** error)
{
  WyreboxDaemonDerivedViewDefinition definition = {
    .view_id = (gchar *) view_id,
    .imap_name = (gchar *) imap_name,
    .definition_ref = (gchar *) definition_ref,
    .rules_source = (gchar *) rules_source,
    .relation_name = (gchar *) relation_name,
  };

  g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return wyrebox_daemon_derived_view_catalog_register_definition
      (self->derived_view_catalog, &definition, error);
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
materialize_configured_derived_views (WyreboxDaemonFactMutationService *self,
    const char *scope_id, GError **error)
{
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;
  g_autoptr (GPtrArray) facts = NULL;
  g_autoptr (GPtrArray) scoped_facts = NULL;
  g_autoptr (GPtrArray) changes = NULL;
  guint n_definitions = 0;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->derived_view_catalog_path == NULL)
    return TRUE;

  n_definitions = wyrebox_daemon_derived_view_catalog_get_n_definitions
      (self->derived_view_catalog);
  if (n_definitions == 0)
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

  for (guint i = 0; i < n_definitions; i++) {
    const WyreboxDaemonDerivedViewDefinition *definition = NULL;
    g_autoptr (GError) view_error = NULL;

    definition = wyrebox_daemon_derived_view_catalog_get_definition
        (self->derived_view_catalog, i);
    if (definition == NULL)
      return FALSE;

    g_clear_pointer (&changes, g_ptr_array_unref);
    if (!wyrebox_derived_view_materializer_refresh_from_rules_and_facts_with_changes (materializer, scope_id, definition->view_id, definition->imap_name, definition->definition_ref, (guint64) g_get_real_time (), definition->rules_source, scoped_facts, definition->relation_name, &changes, &view_error)) {
      g_debug ("derived view materialization failed for %s: %s",
          definition->view_id, view_error != NULL ? view_error->message :
          "unknown error");
      g_propagate_error (error, g_steal_pointer (&view_error));
      return FALSE;
    }

    if (!wyrebox_derived_view_membership_changes_append_journal (changes,
            self->journal_writer, error))
      return FALSE;
  }

  return TRUE;
}

static void
log_materialization_failure_if_needed (WyreboxDaemonFactMutationService *self,
    const char *scope_id, const char *operation_name)
{
  g_autoptr (GError) materialize_error = NULL;

  if (self->derived_view_catalog_path == NULL)
    return;

  if (!materialize_configured_derived_views (self, scope_id,
          &materialize_error)) {
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
    wyrebox_daemon_fact_mutation_service_catch_up_wirelog_derived_view
    (WyreboxDaemonFactMutationService * self, const char *scope_id,
    GError ** error)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (scope_id == NULL || *scope_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view materialization scope_id is required");
    return FALSE;
  }

  if (self->derived_view_journal_root_dir == NULL ||
      self->derived_view_catalog_path == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Wirelog derived view materialization is not configured");
    return FALSE;
  }

  return materialize_configured_derived_views (self, scope_id, error);
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
