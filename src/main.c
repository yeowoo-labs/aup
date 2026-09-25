#define _POSIX_C_SOURCE 200809L
#include "aup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
    fputs("aup check FILE\naup build FILE -o OUT.pmod\n"
          "A library `use name` is name.aup beside FILE or in a directory of AUP_PATH (':' separated).\n", stderr);
    return 2;
}

/* Read a whole source; NULL if it cannot be read. Longer than the limit is
 * left for the compiler to report. */
static char *read_source(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char *source = malloc(AUP_SOURCE_LIMIT + 1);
    *length = source ? fread(source, 1, AUP_SOURCE_LIMIT + 1, file) : 0;
    bool failed = !source || ferror(file);
    fclose(file);
    if (failed) { free(source); return NULL; }
    return source;
}

typedef struct { char directory[AUP_PATH]; char *sources[16]; unsigned count; } Libraries;

static bool try_library(Libraries *l, const char *directory, size_t directory_length, const char *name,
                        const char **source, size_t *length, char path[AUP_PATH]) {
    int written = snprintf(path, AUP_PATH, "%.*s/%s.aup", (int)directory_length, directory, name);
    if (written < 0 || written >= AUP_PATH || l->count == sizeof(l->sources) / sizeof(*l->sources)) return false;
    char *text = read_source(path, length);
    if (!text) return false;
    *source = l->sources[l->count++] = text;
    return true;
}

/* Beside the main source first, then each directory of AUP_PATH in order. */
static bool load_library(void *context, const char *name, const char **source, size_t *length, char path[AUP_PATH]) {
    Libraries *l = context;
    if (try_library(l, l->directory, strlen(l->directory), name, source, length, path)) return true;
    for (const char *at = getenv("AUP_PATH"); at && *at;) {
        size_t span = strcspn(at, ":");
        if (span && try_library(l, at, span, name, source, length, path)) return true;
        at += span + (at[span] == ':');
    }
    return false;
}

int main(int argc, char **argv) {
    bool build = argc == 5 && !strcmp(argv[1], "build") && !strcmp(argv[3], "-o");
    if (!build && !(argc == 3 && !strcmp(argv[1], "check"))) return usage();
    const char *path = argv[2];
    size_t length;
    char *source = read_source(path, &length);
    if (!source) { perror(path); return 2; }
    Libraries libraries = {.count = 0};
    const char *slash = strrchr(path, '/');
    snprintf(libraries.directory, sizeof(libraries.directory), "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
    unsigned char *module;
    size_t size;
    AupDiagnostic error;
    bool compiled = aup_compile(source, length, load_library, &libraries, &module, &size, &error);
    for (unsigned i = 0; i < libraries.count; ++i) free(libraries.sources[i]);
    free(source);
    if (!compiled) {
        fprintf(stderr, "%s:%d:%d: %s\n", *error.path ? error.path : path, error.line, error.column, error.message);
        return 1;
    }
    int status = 0;
    if (build) {
        FILE *out = fopen(argv[4], "wb");
        if (!out || fwrite(module, 1, size, out) != size) { perror(argv[4]); status = 2; }
        if (out && fclose(out)) { perror(argv[4]); status = 2; }
    }
    free(module);
    return status;
}
