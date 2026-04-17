#ifndef OBJECT_H
#define OBJECT_H

#include "pes.h"
#include <stddef.h>
#include <stdint.h>

// Object types for PES-VCS
#define OBJ_BLOB   1
#define OBJ_TREE   2
#define OBJ_COMMIT 3

/**
 * Stores data in the object store.
 * Returns 0 on success, -1 on error.
 */
int object_write(const void *data, size_t len, int type, ObjectID *id_out);

/**
 * Retrieves data from the object store.
 * Returns 0 on success, -1 on error.
 */
int object_read(ObjectID id, void **data_out, size_t *len_out, int *type_out);

#endif
