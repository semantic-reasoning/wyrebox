@0xb60b9f4b0f4c2a9d;

struct RequestFrame {
  identity @0 :RequestIdentity;

  union {
    deliveryIngestion @1 :DeliveryIngestionRequest;
    mailboxList @2 :MailboxListRequest;
    mailboxSelect @3 :MailboxSelectRequest;
    messageFetch @4 :MessageFetchRequest;
    messageSearch @5 :MessageSearchRequest;
    flagKeywordUpdate @6 :FlagKeywordUpdateRequest;
    factMutation @7 :FactMutationRequest;
    wirelogPredicateQuery @8 :WirelogPredicateQueryRequest;
    duckDBQueryTemplate @9 :DuckDBQueryTemplateRequest;
    factBatchImport @10 :FactBatchImportRequest;
  }
}

struct ResponseFrame {
  requestId @0 :Text;
  correlationId @1 :Text;

  union {
    success @2 :SuccessFrame;
    error @3 :ErrorFrame;
    streamChunk @4 :StreamChunkFrame;
    mailboxList @5 :MailboxListResponse;
    mailboxSelect @6 :MailboxSelectResponse;
  }
}

struct RequestIdentity {
  requestId @0 :Text;
  callerIdentity @1 :Text;
  accountIdentity @2 :Text;
  toolIdentity @3 :Text;
  correlationId @4 :Text;
}

struct SuccessFrame {
  requestId @0 :Text;
  durableMarker @1 :Text;
  journalOffset @2 :UInt64;
  summary @3 :Text;
  journalSequence @4 :UInt64;
}

struct ErrorFrame {
  requestId @0 :Text;
  errorClass @1 :ErrorClass;
  message @2 :Text;
  retryHint @3 :Text;
}

struct StreamChunkFrame {
  requestId @0 :Text;
  messageId @1 :Text;
  queryId @2 :Text;
  correlationId @3 :Text;
  chunkIndex @4 :UInt64;
  bytes @5 :Data;
  endOfStream @6 :Bool;
}

enum ErrorClass {
  temporaryFailure @0;
  permanentFailure @1;
  permissionDenied @2;
  notFound @3;
  conflict @4;
  internalError @5;
}

struct DeliveryIngestionRequest {
  deliveryId @0 :Text;
  queueId @1 :Text;
  envelopeSender @2 :Text;
  recipients @3 :List(Text);
  messageBytes @4 :Data;
}

struct MailboxListRequest {
  accountIdentity @0 :Text;
  namespacePrefix @1 :Text;
}

struct MailboxListResponse {
  requestId @0 :Text;
  entries @1 :List(MailboxListEntry);
}

struct MailboxListEntry {
  kind @0 :MailboxListEntryKind;
  mailboxId @1 :Text;
  mailboxName @2 :Text;
  hierarchyDelimiter @3 :Text;
  specialUse @4 :Text;
  selectable @5 :Bool;
  childState @6 :MailboxListChildState;
}

enum MailboxListEntryKind {
  ordinary @0;
  virtual @1;
}

enum MailboxListChildState {
  unknown @0;
  hasChildren @1;
  hasNoChildren @2;
}

struct MailboxSelectRequest {
  accountIdentity @0 :Text;
  mailboxId @1 :Text;
  mailboxName @2 :Text;
}

struct MailboxSelectResponse {
  requestId @0 :Text;
  kind @1 :MailboxListEntryKind;
  mailboxId @2 :Text;
  mailboxName @3 :Text;
  uidValidity @4 :UInt32;
  uidNext @5 :UInt32;
}

struct MessageFetchRequest {
  accountIdentity @0 :Text;
  mailboxId @1 :Text;
  uidValidity @2 :UInt64;
  mailboxUid @3 :UInt64;
}

struct MessageSearchRequest {
  accountIdentity @0 :Text;
  mailboxId @1 :Text;
  uidValidity @2 :UInt64;
  criteriaToken @3 :Text;
}

struct FlagKeywordUpdateRequest {
  accountIdentity @0 :Text;
  mailboxId @1 :Text;
  uidValidity @2 :UInt64;
  mailboxUid @3 :UInt64;
  mode @4 :FlagKeywordUpdateMode;
  systemFlags @5 :List(Text);
  userKeywords @6 :List(Text);
}

enum FlagKeywordUpdateMode {
  set @0;
  clear @1;
  replace @2;
}

struct FactMutationRequest {
  mutation @0 :FactMutationKind;
  predicateId @1 :Text;
  scopeId @2 :Text;
  arguments @3 :List(Text);
}

struct FactBatchImportRequest {
  entries @0 :List(FactMutationRequest);
}

enum FactMutationKind {
  insert @0;
  retract @1;
}

struct WirelogPredicateQueryRequest {
  queryId @0 :Text;
  predicateId @1 :Text;
  scopeId @2 :Text;
  bindings @3 :List(Text);
}

struct DuckDBQueryTemplateRequest {
  queryId @0 :Text;
  templateId @1 :Text;
  scopeId @2 :Text;
  parameters @3 :List(Text);
}
