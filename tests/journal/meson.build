journal_writer_test = executable(
  'test-journal-writer',
  'test-journal-writer.c',
  dependencies: [wyrebox_dep],
)

journal_reader_test = executable(
  'test-journal-reader',
  'test-journal-reader.c',
  dependencies: [wyrebox_dep],
)

message_delivered_payload_test = executable(
  'test-message-delivered-payload',
  'test-message-delivered-payload.c',
  dependencies: [wyrebox_dep],
)

message_delivered_journal_test = executable(
  'test-message-delivered-journal',
  'test-message-delivered-journal.c',
  dependencies: [wyrebox_dep],
)

fact_mutation_journal_test = executable(
  'test-fact-mutation-journal',
  'test-fact-mutation-journal.c',
  dependencies: [wyrebox_dep],
)

test('local journal writer', journal_writer_test)
test('local journal reader', journal_reader_test)
test('message delivered payload codec', message_delivered_payload_test)
test('message delivered journal', message_delivered_journal_test)
test('fact mutation journal', fact_mutation_journal_test)
