#include "wyrebox-daemon-request-adapter.h"
#include "wyrebox-daemon-request-router.h"

#include <gio/gio.h>

struct _WyreboxDaemonRequestAdapter
{
  GObject parent_instance;

  WyreboxDaemonDeliveryIngestionService *delivery_ingestion_service;
  WyreboxDaemonFactMutationService *fact_mutation_service;
  WyreboxDaemonMailboxListService *mailbox_list_service;
  WyreboxDaemonMailboxSelectService *mailbox_select_service;
  WyreboxDaemonMessageFetchService *message_fetch_service;
  WyreboxDaemonMessageSearchService *message_search_service;
  WyreboxDaemonWirelogPredicateQueryService *wirelog_predicate_query_service;
  WyreboxDaemonDuckDBQueryTemplateService *duckdb_query_template_service;
  WyreboxDaemonFlagKeywordUpdateService *flag_keyword_update_service;

  GMutex mutation_route_mutex;

    WyreboxDaemonRequestAdapterDecodeRequestFrameCallback
      decode_request_frame_callback;
  gpointer decode_request_frame_user_data;
  GDestroyNotify decode_request_frame_user_data_destroy;

    WyreboxDaemonRequestAdapterEncodeResponseFrameCallback
      encode_response_frame_callback;
  gpointer encode_response_frame_user_data;
  GDestroyNotify encode_response_frame_user_data_destroy;
};

G_DEFINE_TYPE (WyreboxDaemonRequestAdapter,
    wyrebox_daemon_request_adapter, G_TYPE_OBJECT);

typedef struct
{
  gpointer decoded_state;
  GDestroyNotify decoded_state_clear;
} WyreboxDaemonRequestAdapterDecodedState;

static void
    wyrebox_daemon_request_adapter_decoded_state_clear
    (WyreboxDaemonRequestAdapterDecodedState * state)
{
  if (state == NULL)
    return;

  if (state->decoded_state != NULL && state->decoded_state_clear != NULL)
    state->decoded_state_clear (state->decoded_state);

  state->decoded_state = NULL;
  state->decoded_state_clear = NULL;
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonRequestAdapterDecodedState,
    wyrebox_daemon_request_adapter_decoded_state_clear)
     static void wyrebox_daemon_request_adapter_finalize (GObject *object)
{
  WyreboxDaemonRequestAdapter *self = WYREBOX_DAEMON_REQUEST_ADAPTER (object);

  if (self->decode_request_frame_user_data_destroy != NULL
      && self->decode_request_frame_user_data != NULL)
    self->decode_request_frame_user_data_destroy
        (self->decode_request_frame_user_data);

  if (self->encode_response_frame_user_data_destroy != NULL
      && self->encode_response_frame_user_data != NULL)
    self->encode_response_frame_user_data_destroy
        (self->encode_response_frame_user_data);

  g_clear_object (&self->delivery_ingestion_service);
  g_clear_object (&self->fact_mutation_service);
  g_clear_object (&self->mailbox_list_service);
  g_clear_object (&self->mailbox_select_service);
  g_clear_object (&self->message_fetch_service);
  g_clear_object (&self->message_search_service);
  g_clear_object (&self->wirelog_predicate_query_service);
  g_clear_object (&self->duckdb_query_template_service);
  g_clear_object (&self->flag_keyword_update_service);
  g_mutex_clear (&self->mutation_route_mutex);

  G_OBJECT_CLASS (wyrebox_daemon_request_adapter_parent_class)->finalize
      (object);
}

static void
wyrebox_daemon_request_adapter_class_init (WyreboxDaemonRequestAdapterClass
    *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_request_adapter_finalize;
}

static void
wyrebox_daemon_request_adapter_init (WyreboxDaemonRequestAdapter *self)
{
  g_mutex_init (&self->mutation_route_mutex);
}

static gboolean
operation_requires_mutation_route_lock (WyreboxDaemonRequestFrameOperation
    operation)
{
  switch (operation) {
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DELIVERY_INGESTION:
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION:
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_BATCH_IMPORT:
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FLAG_KEYWORD_UPDATE:
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean
route_decoded_request (WyreboxDaemonRequestAdapter *self,
    const WyreboxDaemonDecodedRequestFrame *decoded_request,
    WyreboxDaemonResponseFrame *response_frame, GError **error)
{
  return wyrebox_daemon_request_router_route (self->delivery_ingestion_service,
      self->fact_mutation_service,
      self->mailbox_list_service,
      self->mailbox_select_service,
      self->message_fetch_service,
      self->message_search_service,
      self->wirelog_predicate_query_service,
      self->duckdb_query_template_service,
      self->flag_keyword_update_service,
      decoded_request, response_frame, error);
}

static gboolean
route_decoded_request_with_mutation_lock (WyreboxDaemonRequestAdapter *self,
    const WyreboxDaemonDecodedRequestFrame *decoded_request,
    WyreboxDaemonResponseFrame *response_frame, GError **error)
{
  gboolean success = FALSE;

  if (!operation_requires_mutation_route_lock (decoded_request->operation))
    return route_decoded_request (self, decoded_request, response_frame, error);

  g_mutex_lock (&self->mutation_route_mutex);
  success = route_decoded_request (self, decoded_request, response_frame,
      error);
  g_mutex_unlock (&self->mutation_route_mutex);

  return success;
}

WyreboxDaemonRequestAdapter *
wyrebox_daemon_request_adapter_new (WyreboxDaemonDeliveryIngestionService
    *delivery_ingestion_service, WyreboxDaemonFactMutationService
    *fact_mutation_service,
    WyreboxDaemonMailboxListService *mailbox_list_service,
    WyreboxDaemonMailboxSelectService *mailbox_select_service,
    WyreboxDaemonMessageFetchService *message_fetch_service,
    WyreboxDaemonMessageSearchService *message_search_service,
    WyreboxDaemonWirelogPredicateQueryService *wirelog_predicate_query_service,
    WyreboxDaemonFlagKeywordUpdateService *flag_keyword_update_service,
    WyreboxDaemonRequestAdapterDecodeRequestFrameCallback
    decode_request_frame_callback, gpointer decode_request_frame_user_data,
    GDestroyNotify decode_request_frame_user_data_destroy,
    WyreboxDaemonRequestAdapterEncodeResponseFrameCallback
    encode_response_frame_callback, gpointer encode_response_frame_user_data,
    GDestroyNotify encode_response_frame_user_data_destroy)
{
  g_return_val_if_fail (decode_request_frame_callback != NULL, NULL);
  g_return_val_if_fail (encode_response_frame_callback != NULL, NULL);
  g_return_val_if_fail (delivery_ingestion_service == NULL
      || WYREBOX_IS_DAEMON_DELIVERY_INGESTION_SERVICE
      (delivery_ingestion_service), NULL);
  g_return_val_if_fail (fact_mutation_service == NULL
      || WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (fact_mutation_service), NULL);
  g_return_val_if_fail (mailbox_list_service == NULL
      || WYREBOX_IS_DAEMON_MAILBOX_LIST_SERVICE (mailbox_list_service), NULL);
  g_return_val_if_fail (mailbox_select_service == NULL
      || WYREBOX_IS_DAEMON_MAILBOX_SELECT_SERVICE (mailbox_select_service),
      NULL);
  g_return_val_if_fail (message_fetch_service == NULL
      || WYREBOX_IS_DAEMON_MESSAGE_FETCH_SERVICE (message_fetch_service), NULL);
  g_return_val_if_fail (message_search_service == NULL
      || WYREBOX_IS_DAEMON_MESSAGE_SEARCH_SERVICE (message_search_service),
      NULL);
  g_return_val_if_fail (wirelog_predicate_query_service == NULL
      || WYREBOX_IS_DAEMON_WIRELOG_PREDICATE_QUERY_SERVICE
      (wirelog_predicate_query_service), NULL);
  g_return_val_if_fail (flag_keyword_update_service == NULL
      || WYREBOX_IS_DAEMON_FLAG_KEYWORD_UPDATE_SERVICE
      (flag_keyword_update_service), NULL);

  WyreboxDaemonRequestAdapter *self =
      g_object_new (WYREBOX_TYPE_DAEMON_REQUEST_ADAPTER, NULL);

  if (delivery_ingestion_service != NULL)
    self->delivery_ingestion_service =
        g_object_ref (delivery_ingestion_service);
  if (fact_mutation_service != NULL)
    self->fact_mutation_service = g_object_ref (fact_mutation_service);

  if (mailbox_list_service != NULL)
    self->mailbox_list_service = g_object_ref (mailbox_list_service);

  if (mailbox_select_service != NULL)
    self->mailbox_select_service = g_object_ref (mailbox_select_service);

  if (message_fetch_service != NULL)
    self->message_fetch_service = g_object_ref (message_fetch_service);
  if (message_search_service != NULL)
    self->message_search_service = g_object_ref (message_search_service);
  if (wirelog_predicate_query_service != NULL)
    self->wirelog_predicate_query_service =
        g_object_ref (wirelog_predicate_query_service);
  if (flag_keyword_update_service != NULL)
    self->flag_keyword_update_service = g_object_ref
        (flag_keyword_update_service);

  self->decode_request_frame_callback = decode_request_frame_callback;
  self->decode_request_frame_user_data = decode_request_frame_user_data;
  self->decode_request_frame_user_data_destroy =
      decode_request_frame_user_data_destroy;
  self->encode_response_frame_callback = encode_response_frame_callback;
  self->encode_response_frame_user_data = encode_response_frame_user_data;
  self->encode_response_frame_user_data_destroy =
      encode_response_frame_user_data_destroy;

  return self;
}

void wyrebox_daemon_request_adapter_set_duckdb_query_template_service
    (WyreboxDaemonRequestAdapter * self,
    WyreboxDaemonDuckDBQueryTemplateService * duckdb_query_template_service)
{
  g_return_if_fail (WYREBOX_IS_DAEMON_REQUEST_ADAPTER (self));
  g_return_if_fail (duckdb_query_template_service == NULL
      || WYREBOX_IS_DAEMON_DUCKDB_QUERY_TEMPLATE_SERVICE
      (duckdb_query_template_service));

  g_set_object (&self->duckdb_query_template_service,
      duckdb_query_template_service);
}

GBytes *
wyrebox_daemon_request_adapter_handle_payload (const
    WyreboxDaemonPeerCredentials *peer_credentials, GBytes *request,
    gpointer user_data, GError **error)
{
  WyreboxDaemonRequestAdapter *self = user_data;
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GBytes) response = NULL;
  g_auto (WyreboxDaemonRequestAdapterDecodedState) decoded_state = { 0 };
  WyreboxDaemonDecodedRequestFrame decoded_request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response_frame = { 0 };

  g_return_val_if_fail (WYREBOX_IS_DAEMON_REQUEST_ADAPTER (self), NULL);
  g_return_val_if_fail (peer_credentials != NULL, NULL);
  g_return_val_if_fail (request != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail (self->decode_request_frame_callback != NULL, NULL);
  g_return_val_if_fail (self->encode_response_frame_callback != NULL, NULL);

  if (!self->decode_request_frame_callback (peer_credentials,
          request,
          &decoded_request,
          &decoded_state.decoded_state,
          &decoded_state.decoded_state_clear,
          self->decode_request_frame_user_data, &local_error))
    goto decode_failed;

  if (!route_decoded_request_with_mutation_lock (self, &decoded_request,
          &response_frame, &local_error))
    goto route_failed;

  response = self->encode_response_frame_callback (&response_frame,
      self->encode_response_frame_user_data, &local_error);
  if (response == NULL)
    goto encode_failed;

  return g_steal_pointer (&response);

decode_failed:
  if (local_error == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "request decode failed without details");
  }
  g_propagate_error (error, g_steal_pointer (&local_error));
  return NULL;

route_failed:
  g_propagate_error (error, g_steal_pointer (&local_error));
  return NULL;

encode_failed:
  if (local_error == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "response encode failed without details");
  }
  g_propagate_error (error, g_steal_pointer (&local_error));
  return NULL;
}
