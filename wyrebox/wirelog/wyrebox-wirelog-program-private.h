#pragma once

#include "wyrebox-wirelog-program.h"

#include <wirelog/wirelog.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

wirelog_program_t *wyrebox_wirelog_program_borrow_wirelog_program (
    WyreboxWirelogProgram *self);

G_END_DECLS
/* *INDENT-ON* */
