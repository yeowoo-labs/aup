#define _POSIX_C_SOURCE 200809L
#include "aup.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int usage(void) {
    fputs("aup check FILE\naup build FILE -o OUT.pmod\naup watch FILE -o OUT.pmod\n"
          "A library `use name` is name.aup beside FILE or in a directory of AUP_PATH (':' separated).\n", stderr);
    return 2;
}

/* Read a whole source (up to one byte past the limit, which the compiler
 * reports); NULL if it cannot be read. */
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

/* Everything a build read: each path it opened or looked for, with a digest
 * of what it found there (0: nothing). A build is a function of these bytes
 * and AUP_PATH alone, so a path that reads differently is a reason to build
 * again and nothing else is. */
enum { READS = 64 };
typedef struct {
    char directory[AUP_PATH], paths[READS][AUP_PATH];
    uint64_t digests[READS];
    unsigned reads;
    bool overflowed; /* it read more paths than READS */
    char *sources[16];
    unsigned count;
} Inputs;

static uint64_t digest(const char *bytes, size_t length) {
    uint64_t h = UINT64_C(0xcbf29ce484222325) ^ length;
    for (size_t i = 0; i < length; ++i) h = (h ^ (unsigned char)bytes[i]) * UINT64_C(0x100000001b3);
    return h ? h : 1;
}

/* read_source, noting in inputs the path and what it held. */
static char *read_input(Inputs *in, const char *path, size_t *length) {
    char *text = read_source(path, length);
    if (in->reads == READS) {
        in->overflowed = true;
    } else {
        snprintf(in->paths[in->reads], AUP_PATH, "%s", path);
        in->digests[in->reads++] = text ? digest(text, *length) : 0;
    }
    return text;
}

static bool try_library(Inputs *in, const char *directory, size_t directory_length, const char *name,
                        const char **source, size_t *length, char path[AUP_PATH]) {
    int written = snprintf(path, AUP_PATH, "%.*s/%s.aup", (int)directory_length, directory, name);
    if (written < 0 || written >= AUP_PATH || in->count == sizeof(in->sources) / sizeof(*in->sources)) return false;
    char *text = read_input(in, path, length);
    if (!text) return false;
    *source = in->sources[in->count++] = text;
    return true;
}

/* Beside the main source first, then each directory of AUP_PATH in order. */
static bool load_library(void *context, const char *name, const char **source, size_t *length, char path[AUP_PATH]) {
    Inputs *l = context;
    if (try_library(l, l->directory, strlen(l->directory), name, source, length, path)) return true;
    for (const char *at = getenv("AUP_PATH"); at && *at;) {
        size_t span = strcspn(at, ":");
        if (span && try_library(l, at, span, name, source, length, path)) return true;
        at += span + (at[span] == ':');
    }
    return false;
}

/* Compile path; write the module to out (unless NULL) through a temporary
 * file renamed over it, so a reader never sees half of one. What it read is
 * left in inputs. 0 on success, 1 on a diagnostic, 2 when a file failed. */
static int compile(const char *path, const char *out, Inputs *inputs) {
    *inputs = (Inputs){.count = 0};
    size_t length;
    char *source = read_input(inputs, path, &length);
    if (!source) { perror(path); return 2; }
    const char *slash = strrchr(path, '/');
    snprintf(inputs->directory, sizeof(inputs->directory), "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
    unsigned char *module;
    size_t size;
    AupDiagnostic error;
    bool compiled = aup_compile(source, length, load_library, inputs, &module, &size, &error);
    for (unsigned i = 0; i < inputs->count; ++i) free(inputs->sources[i]);
    free(source);
    if (!compiled) {
        fprintf(stderr, "%s:%d:%d: %s\n", *error.path ? error.path : path, error.line, error.column, error.message);
        return 1;
    }
    int status = 0;
    if (out) {
        char temporary[AUP_PATH + 32];
        snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", out, (long)getpid());
        FILE *file = fopen(temporary, "wb");
        if (!file || fwrite(module, 1, size, file) != size) { perror(temporary); status = 2; }
        if (file && fclose(file)) { perror(temporary); status = 2; }
        if (!status && rename(temporary, out)) { perror(out); status = 2; }
        if (status) remove(temporary);
    }
    free(module);
    return status;
}

static volatile sig_atomic_t stopping;
static void on_stop(int signal_number) { (void)signal_number; stopping = 1; }

/* One digest of what the paths of the last build hold: as it read them, or
 * as they read now. Comparing bytes, not file times, no write is lost
 * between a build and its bookkeeping, and saving the same bytes is no change. */
static uint64_t contents(const Inputs *in, bool now) {
    uint64_t h = 0;
    for (unsigned i = 0; i < in->reads; ++i) {
        uint64_t d = in->digests[i];
        if (now) {
            size_t length;
            char *text = read_source(in->paths[i], &length);
            d = text ? digest(text, length) : 0;
            free(text);
        }
        h = (h ^ d) * UINT64_C(0x100000001b3);
    }
    return h;
}

/* Build now and again whenever what the last build read reads differently:
 * the source, a library (the one with the error in it too), or a library
 * appearing where one was looked for. A change is built once it reads the
 * same twice in a row, so a file caught halfway through being written is not
 * (a running app swapped to it would lose its states). A failed build keeps
 * the last module. Each outcome is one line on stderr. */
static int watch(const char *path, const char *out) {
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    Inputs inputs;
    uint64_t built = 0, seen = 0;
    for (bool first = true; !stopping; first = false) {
        uint64_t now = first ? 0 : contents(&inputs, true);
        if (first || (now != built && now == seen)) {
            if (!compile(path, out, &inputs)) fprintf(stderr, "aup: built %s\n", out);
            if (inputs.overflowed) fprintf(stderr, "aup: watching only the first %d paths this build read\n", READS);
            built = seen = contents(&inputs, false);
            continue;
        }
        seen = now;
        nanosleep(&(struct timespec){0, 50000000}, NULL);
    }
    return 0;
}

int main(int argc, char **argv) {
    Inputs inputs;
    if (argc == 5 && !strcmp(argv[3], "-o") && !strcmp(argv[1], "build")) return compile(argv[2], argv[4], &inputs);
    if (argc == 5 && !strcmp(argv[3], "-o") && !strcmp(argv[1], "watch")) return watch(argv[2], argv[4]);
    if (argc == 3 && !strcmp(argv[1], "check")) return compile(argv[2], NULL, &inputs);
    return usage();
}
