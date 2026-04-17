// tree.c — Tree object serialization and construction
#include "tree.h"
#include "pes.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── Implemented ────────────────────────────────────────────────────────────

// Local minimal struct to hold parsed index entries (avoids linker dependency)
typedef struct {
    uint32_t mode;
    ObjectID hash;
    char path[512];
} LocalEntry;

static int write_tree_level(LocalEntry *entries, int count, int depth, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *path = entries[i].path;

        // Navigate to current depth
        const char *component = path;
        for (int d = 0; d < depth; d++) {
            component = strchr(component, '/');
            if (!component) return -1;
            component++;
        }

        const char *slash = strchr(component, '/');

        if (slash == NULL) {
            // File at this level
            TreeEntry *entry = &tree.entries[tree.count++];
            entry->mode = entries[i].mode;
            strncpy(entry->name, component, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
            memcpy(entry->hash.hash, entries[i].hash.hash, HASH_SIZE);
            i++;
        } else {
            // Subdirectory — group all entries with same dir prefix
            size_t dir_len = slash - component;
            char dir_name[256];
            strncpy(dir_name, component, dir_len);
            dir_name[dir_len] = '\0';

            int j = i;
            while (j < count) {
                const char *comp2 = entries[j].path;
                for (int d = 0; d < depth; d++) {
                    comp2 = strchr(comp2, '/');
                    if (!comp2) break;
                    comp2++;
                }
                if (!comp2) break;
                const char *sl2 = strchr(comp2, '/');
                if (!sl2) break;
                size_t len2 = sl2 - comp2;
                if (len2 != dir_len || strncmp(comp2, dir_name, dir_len) != 0) break;
                j++;
            }

            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i, depth + 1, &sub_id) < 0)
                return -1;

            TreeEntry *entry = &tree.entries[tree.count++];
            entry->mode = MODE_DIR;
            strncpy(entry->name, dir_name, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
            memcpy(entry->hash.hash, sub_id.hash, HASH_SIZE);

            i = j;
        }
    }

    void *data;
    size_t data_len;
    if (tree_serialize(&tree, &data, &data_len) < 0) return -1;
    int rc = object_write(OBJ_TREE, data, data_len, id_out);
    free(data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    FILE *f = fopen(".pes/index", "r");
    LocalEntry entries[MAX_INDEX_ENTRIES];
    int count = 0;

    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f) && count < MAX_INDEX_ENTRIES) {
            char hex[HASH_HEX_SIZE + 1];
            unsigned long mtime, size, mode;
            char path[512];
            if (sscanf(line, "%lo %64s %lu %lu %511s",
                       &mode, hex, &mtime, &size, path) == 5) {
                entries[count].mode = (uint32_t)mode;
                hex_to_hash(hex, &entries[count].hash);
                strncpy(entries[count].path, path, sizeof(entries[count].path) - 1);
                entries[count].path[sizeof(entries[count].path) - 1] = '\0';
                count++;
            }
        }
        fclose(f);
    }

    if (count == 0) {
        Tree empty;
        empty.count = 0;
        void *data;
        size_t data_len;
        if (tree_serialize(&empty, &data, &data_len) < 0) return -1;
        int rc = object_write(OBJ_TREE, data, data_len, id_out);
        free(data);
        return rc;
    }

    return write_tree_level(entries, count, 0, id_out);
}
