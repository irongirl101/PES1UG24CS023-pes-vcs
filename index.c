// index.c — Staging area implementation

#include "index.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>
#include <time.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

static int cmp_index_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        (size_t)remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── index_load / index_save / index_add ────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        unsigned int mode = 0;
        char hex[HASH_HEX_SIZE + 1];
        unsigned long long mtime_sec = 0;
        unsigned int size = 0;
        char path[512];

        int n = sscanf(line, "%o %64[0-9a-fA-F] %llu %u %511[^\n]",
                       &mode, hex, &mtime_sec, &size, path);
        if (n != 5) {
            fclose(f);
            return -1;
        }

        if (index->count >= MAX_INDEX_ENTRIES) {
            fclose(f);
            return -1;
        }

        IndexEntry *e = &index->entries[index->count++];
        e->mode = mode;
        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }
        e->mtime_sec = mtime_sec;
        e->size = size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }

    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    if (!index || index->count < 0 || index->count > MAX_INDEX_ENTRIES) {
    fprintf(stderr, "index_save: invalid count\n");
    return -1;
    }
    if (mkdir(PES_DIR, 0755) != 0 && errno != EEXIST) return -1;

    size_t n = (size_t)index->count;
    if (n > MAX_INDEX_ENTRIES) return -1;
    IndexEntry *sorted = NULL;
    if (n > 0) {
        sorted = malloc(n * sizeof(IndexEntry));
        if (!sorted) return -1;
        memcpy(sorted, index->entries, n * sizeof(IndexEntry));
        qsort(sorted, n, sizeof(IndexEntry), cmp_index_path);
    }

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        free(sorted);
        return -1;
    }

    for (size_t i = 0; i < n; i++) {
        const IndexEntry *e = &sorted[i];
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&e->hash, hex);
        if (fprintf(f, "%06o %s %llu %u %s\n",
            e->mode,
            hex,
            (unsigned long long)e->mtime_sec,
            e->size,
            e->path) < 0) {
            fclose(f);
            unlink(tmp_path);
            free(sorted);
            return -1;
        }
    }

    if (fflush(f) != 0) {
        fclose(f);
        unlink(tmp_path);
        free(sorted);
        return -1;
    }
    int fd = fileno(f);
    if (fd < 0 || fsync(fd) != 0) {
        fclose(f);
        unlink(tmp_path);
        free(sorted);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(tmp_path);
        free(sorted);
        return -1;
    }

    free(sorted);
    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

int index_add(Index *index, const char *path) {
    printf("STEP 1\n");

    if (!index || !path) return -1;

    printf("STEP 2\n");

    if (index_load(index) < 0) {
        printf("LOAD FAIL\n");
        index->count = 0;
    }

    printf("STEP 3 count=%d\n", index->count);

    struct stat st;
    if (stat(path, &st) != 0) {
        printf("STAT FAIL\n");
        return -1;
    }

    printf("STEP 4 size=%lld\n", (long long)st.st_size);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("FOPEN FAIL\n");
        return -1;
    }

    printf("STEP 5\n");

    size_t size = (size_t)st.st_size;
    void *data = malloc(size ? size : 1);

    printf("STEP 6 data=%p\n", data);

    if (!data) {
        fclose(fp);
        return -1;
    }

    size_t got = fread(data, 1, size, fp);
    fclose(fp);

    printf("STEP 7 read=%zu\n", got);

    ObjectID id;
    printf("STEP 8\n");

    if (object_write(OBJ_BLOB, data, size, &id) < 0) {
        printf("WRITE FAIL\n");
        free(data);
        return -1;
    }

    printf("STEP 9\n");

    free(data);

    printf("STEP 10\n");

    return 0;
}