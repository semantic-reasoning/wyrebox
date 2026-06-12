/*
 * Copyright 2026 WyreBox contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstring>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "wyrebox-daemon-api.capnp.h"

namespace {

static bool
check_request_frame_roundtrip (void)
{
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot<RequestFrame> ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId ("request-id");
  identity.setCallerIdentity ("caller");
  identity.setAccountIdentity ("account");
  identity.setToolIdentity ("tool");
  identity.setCorrelationId ("corr");

  auto mailbox_list = request_frame.initMailboxList ();
  mailbox_list.setAccountIdentity ("account");
  mailbox_list.setNamespacePrefix ("INBOX");

  auto request_words = capnp::messageToFlatArray (request_builder);
  capnp::FlatArrayMessageReader request_reader (request_words.asPtr ());
  auto request_roundtrip = request_reader.getRoot<RequestFrame> ();
  auto request_identity = request_roundtrip.getIdentity ();

  return std::strcmp (request_identity.getRequestId ().cStr (), "request-id") == 0 &&
    std::strcmp (
      request_roundtrip.getMailboxList ().getAccountIdentity ().cStr (),
      "account") == 0 &&
    std::strcmp (request_identity.getCallerIdentity ().cStr (), "caller") == 0;
}

static bool
check_response_frame_roundtrip (void)
{
  capnp::MallocMessageBuilder response_builder;
  auto response_frame = response_builder.initRoot<ResponseFrame> ();

  response_frame.setRequestId ("request-id");
  response_frame.setCorrelationId ("corr");

  auto error_frame = response_frame.initError ();
  error_frame.setRequestId ("request-id");
  error_frame.setErrorClass (ErrorClass::INTERNAL_ERROR);
  error_frame.setMessage ("serialization failed");
  error_frame.setRetryHint ("retry");

  auto response_words = capnp::messageToFlatArray (response_builder);
  capnp::FlatArrayMessageReader response_reader (response_words.asPtr ());
  auto response_roundtrip = response_reader.getRoot<ResponseFrame> ();
  auto response_error = response_roundtrip.getError ();

  return std::strcmp (response_roundtrip.getRequestId ().cStr (), "request-id") == 0 &&
    std::strcmp (response_error.getRequestId ().cStr (), "request-id") == 0 &&
    response_error.getErrorClass () == ErrorClass::INTERNAL_ERROR &&
    std::strcmp (response_error.getMessage ().cStr (), "serialization failed") == 0;
}

} // namespace

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  if (!check_request_frame_roundtrip () ||
      !check_response_frame_roundtrip ()) {
    return 1;
  }

  return 0;
}
