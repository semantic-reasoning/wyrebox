#include "wyrebox-postfix-pipe-input.h"

#include <gio/gio.h>

#define WYREBOX_POSTFIX_PIPE_CALLER_IDENTITY "postfix"
#define WYREBOX_POSTFIX_PIPE_TOOL_IDENTITY "wyrebox-postfix-pipe"
#define WYREBOX_POSTFIX_PIPE_READ_CHUNK_SIZE 8192

typedef struct
{
  char *account_id;
  char *delivery_id;
  char *queue_id;
  char *sender;
  char *socket_path;
  gchar **recipients;
} ParsedArguments;

static void
parsed_arguments_clear (ParsedArguments *arguments)
{
  if (arguments == NULL)
    return;

  g_clear_pointer (&arguments->account_id, g_free);
  g_clear_pointer (&arguments->delivery_id, g_free);
  g_clear_pointer (&arguments->queue_id, g_free);
  g_clear_pointer (&arguments->sender, g_free);
  g_clear_pointer (&arguments->socket_path, g_free);
  g_clear_pointer (&arguments->recipients, g_strfreev);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (ParsedArguments, parsed_arguments_clear)
/* *INDENT-ON* */

static void
clear_outputs (WyreboxPostfixPipeOptions *options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request)
{
  wyrebox_postfix_pipe_options_clear (options);
  wyrebox_daemon_request_identity_clear (identity);
  wyrebox_daemon_delivery_ingestion_request_clear (request);
}

static gboolean
require_non_empty_argument (const char *value, const char *option_name,
    GError **error)
{
  if (value != NULL && *value != '\0')
    return TRUE;

  g_set_error (error,
      G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s is required", option_name);
  return FALSE;
}

static gboolean
require_recipients (const gchar *const *recipients, GError **error)
{
  if (recipients != NULL && recipients[0] != NULL)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "--recipient is required");
  return FALSE;
}

static gchar **
copy_argv (int argc, const char *const *argv)
{
  gchar **copy = g_new0 (gchar *, argc + 1);

  for (int i = 0; i < argc; i++)
    copy[i] = g_strdup (argv[i]);

  return copy;
}

static gboolean
parse_arguments (int argc, const char *const *argv,
    ParsedArguments *arguments, GError **error)
{
  g_auto (GStrv) mutable_argv = NULL;
  g_autoptr (GOptionContext) context = NULL;
  GOptionEntry entries[] = {
    {"account-id", 0, 0, G_OPTION_ARG_STRING, &arguments->account_id,
        "WyreBox account identity", "ACCOUNT"},
    {"delivery-id", 0, 0, G_OPTION_ARG_STRING, &arguments->delivery_id,
        "Postfix delivery identity", "DELIVERY_ID"},
    {"queue-id", 0, 0, G_OPTION_ARG_STRING, &arguments->queue_id,
        "Postfix queue identity", "QUEUE_ID"},
    {"sender", 0, 0, G_OPTION_ARG_STRING, &arguments->sender,
        "Envelope sender", "ENVELOPE_SENDER"},
    {"recipient", 0, 0, G_OPTION_ARG_STRING_ARRAY, &arguments->recipients,
        "Envelope recipient", "RCPT"},
    {"socket", 0, 0, G_OPTION_ARG_STRING, &arguments->socket_path,
        "WyreBox daemon socket path", "PATH"},
    {NULL}
  };

  if (argc <= 0 || argv == NULL) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "argv is required");
    return FALSE;
  }

  mutable_argv = copy_argv (argc, argv);
  context = g_option_context_new (NULL);
  g_option_context_set_help_enabled (context, FALSE);
  g_option_context_add_main_entries (context, entries, NULL);

  if (!g_option_context_parse_strv (context, &mutable_argv, error))
    return FALSE;

  if (!require_non_empty_argument (arguments->account_id, "--account-id",
          error))
    return FALSE;

  if (!require_non_empty_argument (arguments->delivery_id, "--delivery-id",
          error))
    return FALSE;

  if (!require_recipients ((const gchar * const *) arguments->recipients,
          error))
    return FALSE;

  if (mutable_argv[1] != NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "unexpected positional argument: %s", mutable_argv[1]);
    return FALSE;
  }

  if (arguments->sender == NULL)
    arguments->sender = g_strdup ("");

  if (arguments->socket_path == NULL)
    arguments->socket_path =
        g_strdup (WYREBOX_POSTFIX_PIPE_DEFAULT_SOCKET_PATH);

  return TRUE;
}

static gboolean
read_message_bytes (GInputStream *input, gsize max_message_bytes,
    GBytes **out_message_bytes, GError **error)
{
  g_autoptr (GByteArray) bytes = NULL;

  g_return_val_if_fail (out_message_bytes != NULL, FALSE);

  *out_message_bytes = NULL;

  if (input == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "message input stream is required");
    return FALSE;
  }

  bytes = g_byte_array_new ();

  for (;;) {
    guint8 buffer[WYREBOX_POSTFIX_PIPE_READ_CHUNK_SIZE];
    gssize bytes_read;

    bytes_read =
        g_input_stream_read (input, buffer, sizeof (buffer), NULL, error);
    if (bytes_read < 0)
      return FALSE;

    if (bytes_read == 0)
      break;

    if ((gsize) bytes_read > max_message_bytes - bytes->len) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "message exceeds maximum size of %" G_GSIZE_FORMAT " bytes",
          max_message_bytes);
      return FALSE;
    }

    g_byte_array_append (bytes, buffer, (guint) bytes_read);
  }

  if (bytes->len == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "message input must not be empty");
    return FALSE;
  }

  *out_message_bytes = g_byte_array_free_to_bytes (g_steal_pointer (&bytes));
  return TRUE;
}

static char *
build_request_id (const ParsedArguments *arguments)
{
  if (arguments->queue_id == NULL || *arguments->queue_id == '\0')
    return g_strdup_printf ("postfix:%s", arguments->delivery_id);

  return g_strdup_printf ("postfix:%s:%s",
      arguments->queue_id, arguments->delivery_id);
}

void
wyrebox_postfix_pipe_options_clear (WyreboxPostfixPipeOptions *options)
{
  if (options == NULL)
    return;

  g_clear_pointer (&options->socket_path, g_free);
}

gboolean
wyrebox_postfix_pipe_input_build (int argc, const char *const *argv,
    GInputStream *input, gsize max_message_bytes,
    WyreboxPostfixPipeOptions *options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request, GError **error)
{
  g_auto (ParsedArguments) arguments = { 0 };
  g_auto (WyreboxPostfixPipeOptions) next_options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) next_identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) next_request = { 0 };
  g_autoptr (GBytes) message_bytes = NULL;
  g_autofree char *request_id = NULL;

  g_return_val_if_fail (options != NULL, FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  clear_outputs (options, identity, request);

  if (!parse_arguments (argc, argv, &arguments, error))
    return FALSE;

  if (!read_message_bytes (input, max_message_bytes, &message_bytes, error))
    return FALSE;

  request_id = build_request_id (&arguments);
  if (!wyrebox_daemon_request_identity_init (&next_identity,
          request_id,
          WYREBOX_POSTFIX_PIPE_CALLER_IDENTITY,
          arguments.account_id,
          WYREBOX_POSTFIX_PIPE_TOOL_IDENTITY, request_id, error))
    return FALSE;

  if (!wyrebox_daemon_delivery_ingestion_request_init (&next_request,
          arguments.delivery_id,
          arguments.queue_id,
          arguments.sender,
          (const gchar * const *) arguments.recipients, message_bytes, error))
    return FALSE;

  next_options.socket_path = g_strdup (arguments.socket_path);

  *options = next_options;
  *identity = next_identity;
  *request = next_request;
  memset (&next_options, 0, sizeof (next_options));
  memset (&next_identity, 0, sizeof (next_identity));
  memset (&next_request, 0, sizeof (next_request));

  return TRUE;
}
