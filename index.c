#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "index.h"
#include "object.h"

// ---------- FIND ----------
IndexEntry *index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            return &index->entries[i];
        }
    }
    return NULL;
}

// ---------- LOAD ----------
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(".pes/index", "r");
    if (!f) return 0;

    char line[1024];

    while (fgets(line, sizeof(line), f) &&
           index->count < MAX_INDEX_ENTRIES) {

        IndexEntry *e = &index->entries[index->count];

        char hex[HASH_HEX_SIZE + 1];
        unsigned long mode, mtime, size;
        char path[512];

        if (sscanf(line, "%lo %64s %lu %lu %511s",
                   &mode, hex, &mtime, &size, path) == 5) {

            e->mode      = (uint32_t)mode;
            e->mtime_sec = (uint64_t)mtime;
            e->size      = (uint32_t)size;

            hex_to_hash(hex, &e->hash);

            strncpy(e->path, path, sizeof(e->path) - 1);
            e->path[sizeof(e->path) - 1] = '\0';

            index->count++;
        }
    }

    fclose(f);
    return 0;
}

// ---------- SORT ----------
static int compare_index_entries(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

// ---------- SAVE ----------
int index_save(const Index *index) {
    Index sorted = *index;

    qsort(sorted.entries, sorted.count,
          sizeof(IndexEntry), compare_index_entries);

    char tmp_path[] = ".pes/index.tmp.XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) return -1;

    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return -1;
    }

    for (int i = 0; i < sorted.count; i++) {
        char hex[HASH_HEX_SIZE + 1];

        hash_to_hex(&sorted.entries[i].hash, hex);

        fprintf(f, "%o %s %lu %u %s\n",
                sorted.entries[i].mode,
                hex,
                (unsigned long)sorted.entries[i].mtime_sec,
                sorted.entries[i].size,
                sorted.entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp_path, ".pes/index") < 0) return -1;

    int dir_fd = open(".pes", O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return 0;
}

// ---------- ADD ----------
int index_add(Index *index, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    fseek(f, 0, SEEK_SET);

    void *data = malloc(len ? len : 1);
    if (!data) {
        fclose(f);
        return -1;
    }

    if (len > 0 && fread(data, 1, len, f) != len) {
        free(data);
        fclose(f);
        return -1;
    }

    fclose(f);

    // ✅ FIXED ORDER
    ObjectID id;
    if (object_write(data, len, OBJ_BLOB, &id) < 0) {
        free(data);
        return -1;
    }

    free(data);

    struct stat st;
    if (stat(path, &st) < 0) return -1;

    IndexEntry *existing = index_find(index, path);

    if (!existing) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        existing = &index->entries[index->count++];
    }

    if (st.st_mode & 0111)
        existing->mode = 0100755;
    else
        existing->mode = 0100644;

    existing->hash      = id;
    existing->mtime_sec = (uint64_t)st.st_mtime;
    existing->size      = (uint32_t)st.st_size;

    strncpy(existing->path, path, sizeof(existing->path) - 1);
    existing->path[sizeof(existing->path) - 1] = '\0';

    return index_save(index);
}

// ---------- STATUS (CORRECT SIGNATURE) ----------
int index_status(const Index *index) {
    printf("Staged changes:\n");

    if (index->count == 0) {
        printf("(nothing to show)\n");
        return 0;
    }

    for (int i = 0; i < index->count; i++) {
        printf("staged: %s\n", index->entries[i].path);
    }

    return 0;
}
