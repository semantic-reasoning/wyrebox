#ifndef LIB_H
#define LIB_H

#include <stddef.h>
#include <limits.h>

/*
 * Minimal Dovecot compatibility stub used by wyrebox build contract tests.
 */

typedef unsigned long uoff_t;

struct event;
struct array {
  void *dummy;
};
struct mail_storage;

struct pool;
typedef struct pool *pool_t;

typedef struct array array_t;

pool_t pool_alloconly_create (const char *name, size_t size);
void pool_unref (pool_t *pool);
void *p_malloc (pool_t pool, size_t size);
void *p_strdup (pool_t pool, const char *str);
struct event *event_create (struct event *parent);
void event_unref (struct event **event);
void p_array_init (array_t *array, pool_t pool, unsigned int count);

#define p_new(pool, type, count) ((type *) p_malloc ((pool), sizeof(type) * (count)))

#endif
