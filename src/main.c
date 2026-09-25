#define _POSIX_C_SOURCE 200809L
#include "aup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
    fputs("aup check FILE\naup build FILE -o OUT.pmod\n", stderr);
    return 2;
}

int main(int argc, char **argv) {
    bool build = argc == 5 && !strcmp(argv[1], "build") && !strcmp(argv[3], "-o");
    if (!build && !(argc == 3 && !strcmp(argv[1], "check"))) return usage();
    const char *path = argv[2];
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return 2; }
    static char source[AUP_SOURCE_LIMIT + 1];
    size_t length = fread(source, 1, sizeof(source), file);
    bool unreadable = ferror(file);
    fclose(file);
    if (unreadable) { fprintf(stderr, "%s: cannot read\n", path); return 2; }
    unsigned char *module;
    size_t size;
    AupDiagnostic error;
    if (!aup_compile(source, length, &module, &size, &error)) {
        fprintf(stderr, "%s:%d:%d: %s\n", path, error.line, error.column, error.message);
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
