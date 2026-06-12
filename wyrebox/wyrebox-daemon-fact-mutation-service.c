#include "wyrebox-daemon-fact-mutation-service.h"

#include <gio/gio.h>

struct _WyreboxDaemonFactMutationService
{
  GObject parent_instance;

  WyreboxJournalWriter *journal_writer;
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
caller_can_mutate_facts (const char *caller_identity)
{
  return g_strcmp0 (caller_identity, "skill") == 0;
}

static gboolean
authorize_fact_mutation_identity (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFactMutationRequest *request, GError **error)
{
  if (!caller_can_mutate_facts (identity->caller_identity)) {
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

static void
wyrebox_daemon_fact_mutation_service_finalize (GObject *object)
{
  WyreboxDaemonFactMutationService *self =
      WYREBOX_DAEMON_FACT_MUTATION_SERVICE (object);

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
wyrebox_daemon_fact_mutation_service_handle (WyreboxDaemonFactMutationService
    *self, const char *request_id, const char *correlation_id,
    const WyreboxDaemonFactMutationRequest *request,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  guint64 journal_offset = 0;
  guint64 journal_sequence = 0;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (self), FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_request_id (request_id, error))
    return FALSE;

  if (!wyrebox_daemon_fact_mutation_request_append_journal (request,
          self->journal_writer, &journal_offset, &journal_sequence, error))
    return FALSE;

  return wyrebox_daemon_response_frame_init_fact_mutation_success (out_frame,
      request_id, correlation_id, request, journal_offset, journal_sequence,
      error);
}

gboolean
    wyrebox_daemon_fact_mutation_service_handle_identity
    (WyreboxDaemonFactMutationService * self,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonFactMutationRequest * request,
    WyreboxDaemonResponseFrame * out_frame, GError ** error)
{
  g_return_val_if_fail (identity != NULL, FALSE);

  if (!authorize_fact_mutation_identity (identity, request, error))
    return FALSE;

  return wyrebox_daemon_fact_mutation_service_handle (self,
      identity->request_id, identity->correlation_id, request, out_frame,
      error);
}
