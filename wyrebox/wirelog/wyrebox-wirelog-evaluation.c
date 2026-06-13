#include "wyrebox-wirelog-evaluation.h"

#include "wyrebox-wirelog-program-private.h"

#include <gio/gio.h>

#include <wirelog/wirelog.h>

struct _WyreboxWirelogEvaluation
{
  GObject parent_instance;

  WyreboxWirelogProgram *program;
  wirelog_executor_t *executor;
  wirelog_result_t *result;
};

G_DEFINE_TYPE (WyreboxWirelogEvaluation,
    wyrebox_wirelog_evaluation, G_TYPE_OBJECT);

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
wyrebox_wirelog_evaluation_finalize (GObject *object)
{
  WyreboxWirelogEvaluation *self = WYREBOX_WIRELOG_EVALUATION (object);

  g_clear_pointer (&self->result, wirelog_result_free);
  g_clear_pointer (&self->executor, wirelog_executor_free);
  g_clear_object (&self->program);

  G_OBJECT_CLASS (wyrebox_wirelog_evaluation_parent_class)->finalize (object);
}

static void
wyrebox_wirelog_evaluation_class_init (WyreboxWirelogEvaluationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_wirelog_evaluation_finalize;
}

static void
wyrebox_wirelog_evaluation_init (WyreboxWirelogEvaluation *self)
{
}

gboolean
wyrebox_wirelog_program_evaluate (WyreboxWirelogProgram *program,
    WyreboxWirelogEvaluation **out_eval, GError **error)
{
  g_autoptr (WyreboxWirelogEvaluation) self = NULL;
  wirelog_error_t wirelog_error = WIRELOG_OK;
  wirelog_program_t *wirelog_program = NULL;
  wirelog_executor_t *executor = NULL;
  wirelog_result_t *result = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (out_eval == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "Output evaluation pointer is required");
    return FALSE;
  }
  *out_eval = NULL;

  if (program == NULL) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Wirelog program is required");
    return FALSE;
  }

  wirelog_program = wyrebox_wirelog_program_borrow_wirelog_program (program);
  if (wirelog_program == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "Wirelog program handle is not available");
    return FALSE;
  }

  executor = wirelog_executor_create (wirelog_program, &wirelog_error);
  if (executor == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "Failed to create Wirelog executor: %s",
        wirelog_error_name (wirelog_error));
    return FALSE;
  }

  result = wirelog_evaluate (executor, &wirelog_error);
  if (result == NULL) {
    wirelog_executor_free (executor);
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "Failed to evaluate Wirelog program: %s",
        wirelog_error_name (wirelog_error));
    return FALSE;
  }

  self = g_object_new (WYREBOX_TYPE_WIRELOG_EVALUATION, NULL);
  self->program = g_object_ref (program);
  self->executor = executor;
  self->result = result;

  *out_eval = g_steal_pointer (&self);
  return TRUE;
}

gboolean
wyrebox_wirelog_evaluation_get_relation_cardinality (WyreboxWirelogEvaluation
    *self, const char *relation_name, guint64 *out_cardinality, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "Wirelog evaluation is required");
    return FALSE;
  }

  if (relation_name == NULL || relation_name[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "Wirelog relation name is required");
    return FALSE;
  }

  if (out_cardinality == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "Output cardinality pointer is required");
    return FALSE;
  }

  *out_cardinality =
      wirelog_result_relation_cardinality (self->result, relation_name);
  return TRUE;
}
