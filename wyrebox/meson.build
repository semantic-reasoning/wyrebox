wyrebox_sources = files(
  'wyrebox-local-object-store.c',
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
