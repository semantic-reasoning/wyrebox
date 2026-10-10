#pragma once

#include "wyrebox-daemon-config.h"
#include "wyrebox-daemon-wirelog-predicate-query-service.h"
#include "wyrebox-journal-writer.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * The relation a rules file derives virtual mailbox membership into, as
 * show_in_virtual_folder(view_id: symbol, message_id: symbol).
 */
#define WYREBOX_DAEMON_WIRELOG_VIEWS_MEMBERSHIP_RELATION \
  "show_in_virtual_folder"

#define WYREBOX_DAEMON_WIRELOG_VIEWS_DEFAULT_BATCH_SIZE 256

#define WYREBOX_TYPE_DAEMON_WIRELOG_VIEWS \
  (wyrebox_daemon_wirelog_views_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonWirelogViews,
    wyrebox_daemon_wirelog_views,
    WYREBOX,
    DAEMON_WIRELOG_VIEWS,
    GObject)

/*
 * Reads and compiles the Wirelog rules file at @rules_path.
 *
 * Returns: (transfer full): the configured rules, or NULL with
 * G_IO_ERROR_INVALID_DATA when the file cannot be read or does not compile.
 */
WyreboxDaemonWirelogViews *wyrebox_daemon_wirelog_views_new (
    const char *rules_path,
    GError **error);

/*
 * Declares a virtual mailbox @view_id exposed as @imap_name in every account.
 * Its members are the show_in_virtual_folder rows whose view_id is @view_id,
 * evaluated over the facts that @scope names.
 * Fails with G_IO_ERROR_INVALID_DATA when the rules do not declare the
 * membership relation.
 */
gboolean wyrebox_daemon_wirelog_views_add_view (
    WyreboxDaemonWirelogViews *self,
    const char *view_id,
    const char *imap_name,
    WyreboxDaemonViewScope scope,
    GError **error);

/*
 * Opens the DuckDB catalog that holds message_facts and derived view state.
 * Refreshes append DerivedViewMembershipChanged records to @journal_writer,
 * which the views keep a reference to.
 */
gboolean wyrebox_daemon_wirelog_views_open_catalog (
    WyreboxDaemonWirelogViews *self,
    const char *catalog_path,
    WyreboxJournalWriter *journal_writer,
    GError **error);

/*
 * Returns: (transfer full): the accounts known to the catalog, in ascending
 * order.
 */
GStrv wyrebox_daemon_wirelog_views_list_accounts (
    WyreboxDaemonWirelogViews *self,
    GError **error);

/*
 * Evaluates the rules over the active materialized facts of @account_id.
 *
 * Returns: (transfer full) (element-type WyreboxWirelogDerivedMembership):
 * the derived @relation_name rows.
 */
GPtrArray *wyrebox_daemon_wirelog_views_evaluate (
    WyreboxDaemonWirelogViews *self,
    const char *account_id,
    const char *relation_name,
    GError **error);

/*
 * Brings the membership of every configured view of @account_id up to date
 * with its materialized facts, keeping existing virtual UIDs, and journals
 * the membership changes.
 *
 * The catalog records, per account, the journal sequence the last refresh
 * covered and the rules and views it used. A later refresh evaluates only the
 * messages delivered or whose facts changed since then: message-scoped views
 * read the facts of those messages, thread-scoped views the facts of the
 * messages connected to them, and account-scoped views all facts of the
 * account. When there is no record or the rules or views changed, every
 * message is evaluated, in batches of whole threads.
 */
gboolean wyrebox_daemon_wirelog_views_refresh_account (
    WyreboxDaemonWirelogViews *self,
    const char *account_id,
    GError **error);

/*
 * Sets how many messages a refresh evaluates at once; whole threads are never
 * split, so a batch can exceed @batch_size by one thread. Defaults to
 * WYREBOX_DAEMON_WIRELOG_VIEWS_DEFAULT_BATCH_SIZE.
 */
void wyrebox_daemon_wirelog_views_set_batch_size (
    WyreboxDaemonWirelogViews *self,
    guint batch_size);

/*
 * Returns: the number of facts refreshes have loaded from the catalog so far.
 */
guint64 wyrebox_daemon_wirelog_views_get_loaded_fact_count (
    WyreboxDaemonWirelogViews *self);

/*
 * Returns: (transfer full): a predicate query service that evaluates the
 * rules of @self, which it keeps a reference to.
 */
WyreboxDaemonWirelogPredicateQueryService *
wyrebox_daemon_wirelog_views_new_predicate_query_service (
    WyreboxDaemonWirelogViews *self);

G_END_DECLS
/* *INDENT-ON* */
