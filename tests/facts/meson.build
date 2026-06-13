fact_record_test = executable(
  'test-fact-record',
  'test-fact-record.c',
  dependencies: [wyrebox_dep],
)

deterministic_fact_extractor_test = executable(
  'test-deterministic-fact-extractor',
  'test-deterministic-fact-extractor.c',
  dependencies: [wyrebox_dep],
)

fact_reconciliation_test = executable(
  'test-fact-reconciliation',
  'test-fact-reconciliation.c',
  dependencies: [wyrebox_dep],
)

fact_journal_snapshot_test = executable(
  'test-fact-journal-snapshot',
  'test-fact-journal-snapshot.c',
  dependencies: [wyrebox_dep],
)

test('fact record', fact_record_test)
test('deterministic fact extractor', deterministic_fact_extractor_test)
test('fact reconciliation', fact_reconciliation_test)
test('fact journal snapshot', fact_journal_snapshot_test)
