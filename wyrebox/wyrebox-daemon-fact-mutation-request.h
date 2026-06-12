#pragma once

#include "wyrebox-journal-writer.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum
{
  WYREBOX_DAEMON_FACT_MUTATION_INSERT,
  WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
} WyreboxDaemonFactMutationKind;

typedef struct
{
  WyreboxDaemonFactMutationKind mutation;
  char *predicate_id;
  char *scope_id;
  char **arguments;
} WyreboxDaemonFactMutationRequest;

void wyrebox_daemon_fact_mutation_request_clear (
    WyreboxDaemonFactMutationRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonFactMutationRequest,
    wyrebox_daemon_fact_mutation_request_clear)

const char *wyrebox_daemon_fact_mutation_kind_to_wire_name (
    WyreboxDaemonFactMutationKind mutation);

gboolean wyrebox_daemon_fact_mutation_kind_from_wire_name (
    const char *wire_name,
    WyreboxDaemonFactMutationKind *mutation,
    GError **error);

gboolean wyrebox_daemon_fact_mutation_to_event (
    WyreboxDaemonFactMutationKind mutation,
    WyreboxJournalEventType *event_type,
    GError **error);

gboolean wyrebox_daemon_fact_mutation_request_get_event (
    const WyreboxDaemonFactMutationRequest *request,
    WyreboxJournalEventType *event_type,
    GError **error);

gboolean wyrebox_daemon_fact_mutation_request_init (
    WyreboxDaemonFactMutationRequest *request,
    WyreboxDaemonFactMutationKind mutation,
    const char *predicate_id,
    const char *scope_id,
    const char * const *arguments,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
