#pragma once

#include "wyrebox-local-object-store.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DELIVERY_FETCHER (wyrebox_delivery_fetcher_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDeliveryFetcher,
    wyrebox_delivery_fetcher,
    WYREBOX,
    DELIVERY_FETCHER,
    GObject)

typedef enum
{
  WYREBOX_DELIVERY_FETCHER_NAMESPACE_MAILBOX,
  WYREBOX_DELIVERY_FETCHER_NAMESPACE_DERIVED_VIEW,
} WyreboxDeliveryFetcherNamespaceKind;

typedef struct
{
  /*
   * Resolved immutable message identity and raw RFC 5322 bytes.
   * Both fields are owned by the result and are released by clear().
   */
  char *message_id;
  GBytes *bytes;
} WyreboxDeliveryFetchResult;

void wyrebox_delivery_fetch_result_clear (
    WyreboxDeliveryFetchResult *result);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDeliveryFetchResult,
    wyrebox_delivery_fetch_result_clear)

/*
 * Construct a DuckDB-backed read-only delivery fetcher for @catalog_path.
 *
 * @object_store: (transfer none): immutable raw object store used to load
 *   message bytes after the materialized catalog resolves a visible mailbox UID.
 *
 * Returns: (transfer full): caller-owned fetcher, or NULL with @error set.
 */
WyreboxDeliveryFetcher *wyrebox_delivery_fetcher_new_duckdb (
    const gchar *catalog_path,
    WyreboxLocalObjectStore *object_store,
    GError **error);

/*
 * Resolve a visible mailbox UID and return immutable raw RFC 5322 bytes.
 *
 * Returns: (transfer full): message bytes on success, or NULL with @error set.
 */
GBytes *wyrebox_delivery_fetcher_fetch_bytes (
    WyreboxDeliveryFetcher *self,
    const gchar *account_id,
    const gchar *mailbox_id,
    guint64 uidvalidity,
    guint64 uid,
    GError **error);

gboolean wyrebox_delivery_fetcher_fetch_result (
    WyreboxDeliveryFetcher *self,
    const gchar *account_id,
    const gchar *mailbox_id,
    guint64 uidvalidity,
    guint64 uid,
    WyreboxDeliveryFetchResult *out_result,
    GError **error);

/*
 * Resolve a visible namespace UID and return immutable raw RFC 5322 bytes.
 *
 * Returns: (transfer full): message bytes on success, or NULL with @error set.
 */
GBytes *wyrebox_delivery_fetcher_fetch_namespace_bytes (
    WyreboxDeliveryFetcher *self,
    const gchar *account_id,
    WyreboxDeliveryFetcherNamespaceKind namespace_kind,
    const gchar *namespace_id,
    guint64 uidvalidity,
    guint64 uid,
    GError **error);

gboolean wyrebox_delivery_fetcher_fetch_namespace_result (
    WyreboxDeliveryFetcher *self,
    const gchar *account_id,
    WyreboxDeliveryFetcherNamespaceKind namespace_kind,
    const gchar *namespace_id,
    guint64 uidvalidity,
    guint64 uid,
    WyreboxDeliveryFetchResult *out_result,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
