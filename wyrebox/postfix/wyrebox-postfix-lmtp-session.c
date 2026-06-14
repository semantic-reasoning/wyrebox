#include "wyrebox-postfix-lmtp-session.h"

#include <gio/gio.h>
#include <string.h>

#define WYREBOX_POSTFIX_LMTP_CALLER_IDENTITY "postfix"
#define WYREBOX_POSTFIX_LMTP_TOOL_IDENTITY "wyrebox-postfix-lmtp"
#define WYREBOX_POSTFIX_LMTP_MAX_COMMAND_LINE_BYTES 8192

typedef struct
{
  char *account_id;
  char *delivery_id;
  char *socket_path;
} ParsedArguments;

typedef struct
{
  gboolean seen_lhlo;
  gboolean seen_mail;
  char *sender;
  GPtrArray *recipients;
  GByteArray *message;
} LmtpTransaction;

static void
parsed_arguments_clear (ParsedArguments *arguments)
{
  if (arguments == NULL)
    return;

  g_clear_pointer (&arguments->account_id, g_free);
  g_clear_pointer (&arguments->delivery_id, g_free);
  g_clear_pointer (&arguments->socket_path, g_free);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (ParsedArguments, parsed_arguments_clear)
/* *INDENT-ON* */

static void
lmtp_transaction_clear (LmtpTransaction *transaction)
{
  if (transaction == NULL)
    return;

  g_clear_pointer (&transaction->sender, g_free);
  g_clear_pointer (&transaction->recipients, g_ptr_array_unref);
  g_clear_pointer (&transaction->message, g_byte_array_unref);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (LmtpTransaction, lmtp_transaction_clear)
/* *INDENT-ON* */

static void
clear_outputs (WyreboxPostfixLmtpOptions *options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request)
{
  wyrebox_postfix_lmtp_options_clear (options);
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
        "Stable Postfix delivery identity", "DELIVERY_ID"},
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

  if (mutable_argv[1] != NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "unexpected positional argument: %s", mutable_argv[1]);
    return FALSE;
  }

  if (arguments->socket_path == NULL)
    arguments->socket_path =
        g_strdup (WYREBOX_POSTFIX_LMTP_DEFAULT_SOCKET_PATH);

  return TRUE;
}

static gboolean
read_crlf_line (GInputStream *input,
    GByteArray *line, gsize max_line_bytes, gboolean *out_eof, GError **error)
{
  gboolean pending_cr = FALSE;

  g_return_val_if_fail (line != NULL, FALSE);
  g_return_val_if_fail (out_eof != NULL, FALSE);

  if (input == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "LMTP input stream is required");
    return FALSE;
  }

  *out_eof = FALSE;
  g_byte_array_set_size (line, 0);

  for (;;) {
    guint8 byte = 0;
    gssize bytes_read =
        g_input_stream_read (input, &byte, sizeof byte, NULL, error);

    if (bytes_read < 0)
      return FALSE;
    if (bytes_read == 0) {
      if (line->len == 0 && !pending_cr) {
        *out_eof = TRUE;
        return TRUE;
      }

      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT, "LMTP input has unterminated line");
      return FALSE;
    }

    if (pending_cr) {
      if (byte == '\n')
        return TRUE;

      if (line->len >= max_line_bytes) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "LMTP line exceeds maximum size of %" G_GSIZE_FORMAT " bytes",
            max_line_bytes);
        return FALSE;
      }
      g_byte_array_append (line, (const guint8 *) "\r", 1);
      pending_cr = FALSE;
    }

    if (byte == '\r') {
      pending_cr = TRUE;
      continue;
    }

    if (line->len >= max_line_bytes) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "LMTP line exceeds maximum size of %" G_GSIZE_FORMAT " bytes",
          max_line_bytes);
      return FALSE;
    }
    g_byte_array_append (line, &byte, 1);
  }
}

static gboolean
line_has_prefix_ci (const char *line, const char *prefix)
{
  gsize prefix_len = strlen (prefix);

  return g_ascii_strncasecmp (line, prefix, prefix_len) == 0;
}

static char *
extract_path_argument (const char *line, const char *command, GError **error)
{
  const char *start = NULL;
  const char *end = NULL;

  if (!line_has_prefix_ci (line, command)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "LMTP command does not match %s", command);
    return NULL;
  }

  start = strchr (line, '<');
  end = strrchr (line, '>');
  if (start == NULL || end == NULL || end < start) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "LMTP command %s requires angle address",
        command);
    return NULL;
  }

  return g_strndup (start + 1, (gsize) (end - start - 1));
}

static gboolean
lmtp_transaction_add_data_line (LmtpTransaction *transaction,
    const guint8 *line, gsize line_len, gsize max_message_bytes, GError **error)
{
  const guint8 *payload = line;
  gsize payload_len = line_len;
  gsize current_len = transaction->message->len;

  if (line_len > 0 && line[0] == '.') {
    payload = line + 1;
    payload_len = line_len - 1;
  }

  if (current_len > max_message_bytes ||
      payload_len > max_message_bytes - current_len ||
      2 > max_message_bytes - current_len - payload_len) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "LMTP DATA exceeds maximum size of %" G_GSIZE_FORMAT " bytes",
        max_message_bytes);
    return FALSE;
  }

  g_byte_array_append (transaction->message, payload, (guint) payload_len);
  g_byte_array_append (transaction->message, (const guint8 *) "\r\n", 2);
  return TRUE;
}

static gboolean
parse_lmtp_conversation (GInputStream *input, gsize max_message_bytes,
    LmtpTransaction *transaction, GError **error)
{
  gboolean in_data = FALSE;
  g_autoptr (GByteArray) raw_line = NULL;

  g_return_val_if_fail (transaction != NULL, FALSE);

  transaction->recipients = g_ptr_array_new_with_free_func (g_free);
  transaction->message = g_byte_array_new ();
  raw_line = g_byte_array_new ();

  for (;;) {
    gsize max_line_bytes = WYREBOX_POSTFIX_LMTP_MAX_COMMAND_LINE_BYTES;
    gboolean eof = FALSE;
    g_autofree char *line = NULL;

    if (in_data) {
      gsize current_len = transaction->message->len;
      gsize remaining = 0;

      if (current_len > max_message_bytes) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "LMTP DATA exceeds maximum size of %" G_GSIZE_FORMAT " bytes",
            max_message_bytes);
        return FALSE;
      }

      remaining = max_message_bytes - current_len;
      max_line_bytes = remaining < 2 ? 1 : remaining - 1;
    }

    if (!read_crlf_line (input, raw_line, max_line_bytes, &eof, error))
      return FALSE;
    if (eof)
      break;

    if (in_data) {
      if (raw_line->len == 1 && raw_line->data[0] == '.') {
        in_data = FALSE;
        continue;
      }

      if (!lmtp_transaction_add_data_line (transaction,
              raw_line->data, raw_line->len, max_message_bytes, error))
        return FALSE;
      continue;
    }

    line = g_strndup ((const char *) raw_line->data, raw_line->len);

    if (line_has_prefix_ci (line, "LHLO ")) {
      transaction->seen_lhlo = TRUE;
      continue;
    }

    if (line_has_prefix_ci (line, "MAIL FROM:")) {
      if (!transaction->seen_lhlo || transaction->seen_mail) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT, "LMTP MAIL command is out of order");
        return FALSE;
      }

      transaction->sender = extract_path_argument (line, "MAIL FROM:", error);
      if (transaction->sender == NULL)
        return FALSE;
      transaction->seen_mail = TRUE;
      continue;
    }

    if (line_has_prefix_ci (line, "RCPT TO:")) {
      g_autofree char *recipient = NULL;

      if (!transaction->seen_mail) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT, "LMTP RCPT command is out of order");
        return FALSE;
      }

      if (transaction->recipients->len > 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "LMTP first slice supports exactly one recipient");
        return FALSE;
      }

      recipient = extract_path_argument (line, "RCPT TO:", error);
      if (recipient == NULL)
        return FALSE;
      g_ptr_array_add (transaction->recipients, g_steal_pointer (&recipient));
      continue;
    }

    if (g_ascii_strcasecmp (line, "DATA") == 0) {
      if (transaction->recipients->len != 1) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "LMTP DATA requires exactly one accepted recipient");
        return FALSE;
      }

      in_data = TRUE;
      continue;
    }

    if (g_ascii_strcasecmp (line, "RSET") == 0) {
      g_clear_pointer (&transaction->sender, g_free);
      g_ptr_array_set_size (transaction->recipients, 0);
      g_byte_array_set_size (transaction->message, 0);
      transaction->seen_mail = FALSE;
      in_data = FALSE;
      continue;
    }

    if (g_ascii_strcasecmp (line, "QUIT") == 0)
      break;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "unsupported LMTP command: %s", line);
    return FALSE;
  }

  if (in_data) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "LMTP DATA was not terminated");
    return FALSE;
  }

  if (!transaction->seen_mail || transaction->recipients->len != 1 ||
      transaction->message->len == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "LMTP transaction is incomplete");
    return FALSE;
  }

  return TRUE;
}

static char *
build_request_id (const ParsedArguments *arguments)
{
  return g_strdup_printf ("postfix-lmtp:%s", arguments->delivery_id);
}

void
wyrebox_postfix_lmtp_options_clear (WyreboxPostfixLmtpOptions *options)
{
  if (options == NULL)
    return;

  g_clear_pointer (&options->socket_path, g_free);
}

gboolean
wyrebox_postfix_lmtp_session_build (int argc, const char *const *argv,
    GInputStream *input, gsize max_message_bytes,
    WyreboxPostfixLmtpOptions *options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request, GError **error)
{
  g_auto (ParsedArguments) arguments = { 0 };
  g_auto (LmtpTransaction) transaction = { 0 };
  g_auto (WyreboxPostfixLmtpOptions) next_options = { 0 };
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

  if (!parse_lmtp_conversation (input, max_message_bytes, &transaction, error))
    return FALSE;

  request_id = build_request_id (&arguments);
  if (!wyrebox_daemon_request_identity_init (&next_identity,
          request_id,
          WYREBOX_POSTFIX_LMTP_CALLER_IDENTITY,
          arguments.account_id,
          WYREBOX_POSTFIX_LMTP_TOOL_IDENTITY, request_id, error))
    return FALSE;

  message_bytes = g_byte_array_free_to_bytes (g_steal_pointer
      (&transaction.message));

  {
    const gchar *recipients[] = {
      g_ptr_array_index (transaction.recipients, 0),
      NULL
    };

    if (!wyrebox_daemon_delivery_ingestion_request_init (&next_request,
            arguments.delivery_id,
            NULL, transaction.sender, recipients, message_bytes, error))
      return FALSE;
  }

  next_options.socket_path = g_steal_pointer (&arguments.socket_path);

  wyrebox_postfix_lmtp_options_clear (options);
  wyrebox_daemon_request_identity_clear (identity);
  wyrebox_daemon_delivery_ingestion_request_clear (request);
  *options = next_options;
  *identity = next_identity;
  *request = next_request;
  memset (&next_options, 0, sizeof next_options);
  memset (&next_identity, 0, sizeof next_identity);
  memset (&next_request, 0, sizeof next_request);

  return TRUE;
}
