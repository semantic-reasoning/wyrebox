wyrebox_sources = files(
  'wyrebox-local-object-store.c',
  'wyrebox-delivery-projection.c',
  'wyrebox-delivery-replay-validator.c',
  'wyrebox-eml-ingestor.c',
  'wyrebox-eml-metadata.c',
  'wyrebox-message-delivered-payload.c',
  'wyrebox-journal-writer.c',
  'wyrebox-journal-reader.c',
  'wyrebox-schema-migration.c',
)

wyrebox_inc = include_directories('.')

wyrebox_lib = static_library(
  'wyrebox',
  wyrebox_sources,
  include_directories: wyrebox_inc,
  dependencies: [glib_dep, gobject_dep, gio_dep],
)

wyrebox_dep = declare_dependency(
  include_directories: wyrebox_inc,
  link_with: wyrebox_lib,
  dependencies: [glib_dep, gobject_dep, gio_dep],
)
