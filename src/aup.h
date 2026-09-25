#ifndef AUP_H
#define AUP_H
#include <stdbool.h>
#include <stddef.h>

/* aup: source text to a Pure module (Pure docs/design/008-module-format.md). */
enum { AUP_SOURCE_LIMIT = 32768, AUP_MESSAGE = 160 };

typedef struct {
    int line, column;
    char message[AUP_MESSAGE];
} AupDiagnostic;

/* Compile source into a newly allocated module (free with free()). The same
 * source always produces the same bytes. On failure *module is NULL and the
 * diagnostic names the first error. */
bool aup_compile(const char *source, size_t length, unsigned char **module, size_t *size,
                 AupDiagnostic *error);
#endif
