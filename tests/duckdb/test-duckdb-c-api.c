/*
 * Copyright (C) 2026
 */

#include <duckdb.h>
#include <stddef.h>

int
main (void)
{
  duckdb_database db = NULL;

  if (duckdb_open (NULL, &db) != DuckDBSuccess)
    return 1;

  duckdb_close (&db);

  return 0;
}
