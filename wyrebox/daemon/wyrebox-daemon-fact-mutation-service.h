#pragma once

#include "wyrebox-daemon-fact-batch-import-request.h"
#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"
#include "wyrebox-daemon-derived-view-catalog.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_FACT_MUTATION_SERVICE \
  (wyrebox_daemon_fact_mutation_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonFactMutationService,
    wyrebox_daemon_fact_mutation_service,
    WYREBOX,
    DAEMON_FACT_MUTATION_SERVICE,
    GObject)

WyreboxDaemonFactMutationService *wyrebox_daemon_fact_mutation_service_new (
    WyreboxJournalWriter *journal_writer);

gboolean wyrebox_daemon_fact_mutation_service_configure_wirelog_derived_view (
    WyreboxDaemonFactMutationService *self,
    const char *journal_root_dir,
    const char *catalog_path,
    GError **error);

gboolean wyrebox_daemon_fact_mutation_service_register_wirelog_derived_view (
    WyreboxDaemonFactMutationService *self,
    const char *view_id,
    const char *imap_name,
    const char *definition_ref,
    const char *rules_source,
    const char *relation_name,
    GError **error);

gboolean wyrebox_daemon_fact_mutation_service_enable_wirelog_derived_view (
    WyreboxDaemonFactMutationService *self,
    const char *view_id,
    GError **error);

gboolean wyrebox_daemon_fact_mutation_service_disable_wirelog_derived_view (
    WyreboxDaemonFactMutationService *self,
    const char *view_id,
    GError **error);

gboolean wyrebox_daemon_fact_mutation_service_catch_up_wirelog_derived_view (
    WyreboxDaemonFactMutationService *self,
    const char *scope_id,
    GError **error);

gboolean wyrebox_daemon_fact_mutation_service_handle_identity (
    WyreboxDaemonFactMutationService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFactMutationRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

gboolean wyrebox_daemon_fact_mutation_service_handle_batch_identity (
    WyreboxDaemonFactMutationService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFactBatchImportRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
