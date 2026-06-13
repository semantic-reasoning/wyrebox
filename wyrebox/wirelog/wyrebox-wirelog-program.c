#include "wyrebox-wirelog-program.h"

#include <gio/gio.h>

#include <wirelog/wirelog.h>

struct _WyreboxWirelogProgram
{
  GObject parent_instance;

  wirelog_program_t *program;
};

G_DEFINE_TYPE (WyreboxWirelogProgram, wyrebox_wirelog_program, G_TYPE_OBJECT);

static const char *
wirelog_error_name (wirelog_error_t error)
{
  switch (error) {
    case WIRELOG_OK:
      return "ok";
    case WIRELOG_ERR_PARSE:
      return "parse error";
    case WIRELOG_ERR_INVALID_IR:
      return "invalid IR";
    case WIRELOG_ERR_EXEC:
      return "execution error";
    case WIRELOG_ERR_MEMORY:
      return "memory error";
    case WIRELOG_ERR_IO:
      return "I/O error";
    case WIRELOG_ERR_COMPOUND_SATURATED:
      return "compound arena saturated";
    case WIRELOG_ERR_COMPOUND_BUSY:
      return "compound arena busy";
    case WIRELOG_ERR_UNKNOWN:
      return "unknown error";
    default:
      return "unrecognized error";
  }
}

static void
wyrebox_wirelog_program_finalize (GObject *object)
{
  WyreboxWirelogProgram *self = WYREBOX_WIRELOG_PROGRAM (object);

  g_clear_pointer (&self->program, wirelog_program_free);

  G_OBJECT_CLASS (wyrebox_wirelog_program_parent_class)->finalize (object);
}

static void
wyrebox_wirelog_program_class_init (WyreboxWirelogProgramClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_wirelog_program_finalize;
}

static void
wyrebox_wirelog_program_init (WyreboxWirelogProgram *self)
{
}

WyreboxWirelogProgram *
wyrebox_wirelog_program_new_from_source (const char *source, GError **error)
{
  g_autoptr (WyreboxWirelogProgram) self = NULL;
  wirelog_error_t wirelog_error = WIRELOG_OK;
  wirelog_program_t *program = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (source == NULL || source[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "Wirelog source text is required");
    return NULL;
  }

  program = wirelog_parse_string (source, &wirelog_error);
  if (program == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Failed to parse Wirelog source: %s",
        wirelog_error_name (wirelog_error));
    return NULL;
  }

  self = g_object_new (WYREBOX_TYPE_WIRELOG_PROGRAM, NULL);
  self->program = program;

  return g_steal_pointer (&self);
}
