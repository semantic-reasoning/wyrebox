#include "wyrebox-postfix-lmtp-delivery-bridge.h"

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <sysexits.h>

static int
exit_status_for_reply (const WyreboxPostfixLmtpReply *reply)
{
  switch (reply->outcome) {
    case WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_DELIVERED:
      return EX_OK;
    case WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_PERMANENT_FAILURE:
      return EX_DATAERR;
    case WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE:
    default:
      return EX_TEMPFAIL;
  }
}

static void
write_reply_line (const WyreboxPostfixLmtpReply *reply)
{
  g_print ("%d %s %s\r\n",
      reply->reply_code, reply->enhanced_status, reply->reply_text);
}

static void
write_reply_log (const WyreboxPostfixLmtpReply *reply)
{
  g_printerr ("%s\n", reply->log_message);
}

static int
return_reply (const WyreboxPostfixLmtpReply *reply)
{
  write_reply_line (reply);
  write_reply_log (reply);
  return exit_status_for_reply (reply);
}

static int
return_local_error (const WyreboxDaemonRequestIdentity *identity,
    const GError *cause)
{
  g_auto (WyreboxPostfixLmtpReply) reply = { 0 };

  if (!wyrebox_postfix_lmtp_reply_init_from_delivery_error (&reply,
          identity != NULL ? identity->request_id : NULL,
          identity != NULL ? identity->correlation_id : NULL, cause, NULL)) {
    g_print ("451 4.3.0 Temporary local delivery failure\r\n");
    g_printerr ("postfix_lmtp outcome=temporary_failure reply_code=451 "
        "enhanced_status=4.3.0 request_id=<none> correlation_id=<none> "
        "failure_source=local failure_class=replyConstruction\n");
    return EX_TEMPFAIL;
  }

  return return_reply (&reply);
}

int
main (int argc, char **argv)
{
  g_autoptr (GInputStream) stdin_stream = NULL;
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxPostfixLmtpReply) reply = { 0 };
  g_autoptr (GError) error = NULL;

  stdin_stream = g_unix_input_stream_new (STDIN_FILENO, FALSE);
  if (!wyrebox_postfix_lmtp_session_build (argc,
          (const char *const *) argv,
          stdin_stream, G_MAXSIZE, &options, &identity, &request, &error))
    return return_local_error (&identity, error);

  if (!wyrebox_postfix_lmtp_delivery_bridge_deliver (&options,
          &identity, &request, &reply, &error))
    return return_local_error (&identity, error);

  return return_reply (&reply);
}
