#ifndef AUP_H
#define AUP_H
#include <stdbool.h>
#include <stddef.h>

/* aup: source text to a Pure module (Pure docs/design/008-module-format.md). */
enum { AUP_MESSAGE = 256, AUP_PATH = 256 };

typedef struct {
    char path[AUP_PATH]; /* the library the error is in, or "" for the main source */
    int line, column;
    char message[AUP_MESSAGE];
} AupDiagnostic;

/* Finds the library that `use name` names: its source, alive until
 * aup_compile returns, and a path that diagnostics show. */
typedef bool (*AupLoad)(void *context, const char *name, const char **source, size_t *length,
                        char path[AUP_PATH]);

/* Compile source into a newly allocated module (free with free()). The same
 * sources always produce the same bytes. On failure *module is NULL and the
 * diagnostic names the first error. load may be NULL when nothing is used. */
bool aup_compile(const char *source, size_t length, AupLoad load, void *context,
                 unsigned char **module, size_t *size, AupDiagnostic *error);
#endif
