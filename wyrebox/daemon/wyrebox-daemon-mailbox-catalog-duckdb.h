#pragma once

#include "wyrebox-daemon-mailbox-list-service.h"
#include "wyrebox-daemon-mailbox-select-service.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct _WyreboxDaemonMailboxCatalogDuckDB
    WyreboxDaemonMailboxCatalogDuckDB;

WyreboxDaemonMailboxCatalogDuckDB *
wyrebox_daemon_mailbox_catalog_duckdb_new (
    const char *catalog_path,
    GError **error);

void wyrebox_daemon_mailbox_catalog_duckdb_free (
    WyreboxDaemonMailboxCatalogDuckDB *catalog);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (WyreboxDaemonMailboxCatalogDuckDB,
    wyrebox_daemon_mailbox_catalog_duckdb_free)

gboolean wyrebox_daemon_mailbox_catalog_duckdb_list (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request,
    WyreboxDaemonMailboxListResult *out_result,
    gpointer user_data,
    GError **error);

gboolean wyrebox_daemon_mailbox_catalog_duckdb_select (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxSelectRequest *request,
    WyreboxDaemonMailboxSelectResult *out_result,
    gpointer user_data,
    GError **error);

WyreboxDaemonMailboxListService *
wyrebox_daemon_mailbox_catalog_duckdb_new_list_service (
    const char *catalog_path,
    GError **error);

WyreboxDaemonMailboxSelectService *
wyrebox_daemon_mailbox_catalog_duckdb_new_select_service (
    const char *catalog_path,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
