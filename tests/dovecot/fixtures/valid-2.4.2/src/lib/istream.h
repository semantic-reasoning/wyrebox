#ifndef ISTREAM_H
#define ISTREAM_H

#include <stdbool.h>

#include "lib.h"

struct istream
{
  void *data;
  unsigned int size;
  bool owns_data;
};

struct istream *i_stream_create_from_data (const void *data, size_t size);
struct istream *i_stream_create_copy_from_data (const void *data, size_t size);
void i_stream_unref (struct istream **stream);

#endif
