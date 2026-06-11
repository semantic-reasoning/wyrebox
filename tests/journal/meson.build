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

test('local journal writer', journal_writer_test)
test('local journal reader', journal_reader_test)
