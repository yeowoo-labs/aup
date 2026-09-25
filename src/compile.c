#include "aup.h"
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Limits and opcodes of Pure module format v2. */
enum { STATES = 32, INPUTS = 8, VIEWS = 24, HANDLERS = 64, CODE = 512, STACK = 64, NAME = 32,
       DEPTH = 64, FIELDS = 32, COLLECTIONS = 1 };
enum { CONST, LOAD, DT, ADD, SUB, MUL, DIV, NEG, LT, LE, GT, GE, EQ, NE, AND, OR, NOT,
       MIN, MAX, CLAMP, STORE, JZ, JMP, FIELD, DOC, DOC_SIZE, LEN, REPLACE, REPLY, REQUIRE };
enum { KIND_KEY = 1, KIND_MESSAGE = 2, FIELD_U32 = 1, FIELD_BYTES = 2 };
typedef enum { NUM = 1, BYTES = 2 } Type;

typedef enum { T_EOF, T_NL, T_NUMBER, T_NAME, T_PUNCT } TokenKind;
typedef struct {
    TokenKind kind;
    int line, column;
    double number;
    char text[NAME + 1]; /* name, or punctuation of one or two characters */
} Token;

typedef enum {
    E_NUMBER, E_NAME, E_DT, E_UNARY, E_BINARY, E_CALL,
    E_FIELD, /* name.member */
    E_DOC,   /* name[key] */
    E_SIZE,  /* name[key].bytes */
    E_LEN,   /* value.bytes */
} ExprKind;
typedef struct Expr {
    ExprKind kind;
    int line, column, op;
    double number;
    char name[NAME], member[NAME];
    struct Expr *args[3];
    unsigned count;
} Expr;

typedef enum { S_ASSIGN, S_IF, S_REPLACE, S_REPLY, S_REQUIRE } StmtKind;
typedef struct Stmt {
    StmtKind kind;
    int line, column;
    char name[NAME];
    Expr *value;          /* assigned value, condition, reply, requirement or insertion */
    Expr *first, *last;   /* replace range */
    struct Stmt *then, *otherwise, *next;
} Stmt;

typedef struct {
    char name[NAME], key[NAME];
    int kind;
    bool keyed;
    char field_names[FIELDS][NAME];
    int field_types[FIELDS];
    unsigned fields;
} Input;

typedef struct {
    const char *source;
    size_t length, at;
    int line, column;
    Token token, ahead;
    bool has_ahead;
    unsigned depth;
    jmp_buf fail;
    AupDiagnostic *error;
    void **blocks;
    size_t block_count, block_capacity;
    /* Declarations in source order. */
    char state_names[STATES][NAME];
    double initial[STATES];
    unsigned states;
    char collection_names[COLLECTIONS][NAME];
    unsigned collections;
    Input inputs[INPUTS];
    unsigned input_count, field_count;
    char view_names[VIEWS][NAME];
    Expr *view_value[VIEWS];
    unsigned views;
    struct { char input[NAME]; int edge, line, column; Stmt *body; } handlers[HANDLERS];
    unsigned handler_count;
    /* Code generation. */
    int message;          /* input of the handler being generated, or -1 */
    struct { uint8_t op; uint32_t arg; double value; } code[CODE];
    unsigned code_count;
    uint32_t view_code[VIEWS][2], handler_code[HANDLERS][2];
} Compiler;

static const char *const reserved[] = {"state", "input", "view", "on", "if", "else", "key", "frame",
                                       "dt", "min", "max", "clamp", "message", "by", "bytes", "u32",
                                       "reply", "require"};

static _Noreturn void fail_at(Compiler *c, int line, int column, const char *format, ...) {
    va_list args;
    va_start(args, format);
    c->error->line = line;
    c->error->column = column;
    vsnprintf(c->error->message, sizeof(c->error->message), format, args);
    va_end(args);
    longjmp(c->fail, 1);
}

static void *allocate(Compiler *c, size_t size) {
    if (c->block_count == c->block_capacity) {
        size_t capacity = c->block_capacity ? 2 * c->block_capacity : 64;
        void **blocks = realloc(c->blocks, capacity * sizeof(*blocks));
        if (!blocks) fail_at(c, c->line, c->column, "out of memory");
        c->blocks = blocks;
        c->block_capacity = capacity;
    }
    void *block = calloc(1, size);
    if (!block) fail_at(c, c->line, c->column, "out of memory");
    return c->blocks[c->block_count++] = block;
}

/* ---- Lexer ---------------------------------------------------------- */

static char peek_char(const Compiler *c, size_t offset) {
    return c->at + offset < c->length ? c->source[c->at + offset] : 0;
}

static void advance(Compiler *c) {
    if (c->source[c->at++] == '\n') { ++c->line; c->column = 1; } else ++c->column;
}

static bool digit(char ch) { return ch >= '0' && ch <= '9'; }
static bool letter(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }

static Token scan(Compiler *c) {
    for (;;) {
        char ch = peek_char(c, 0);
        if (ch == ' ' || ch == '\t' || ch == '\r') advance(c);
        else if (ch == '#') while (c->at < c->length && peek_char(c, 0) != '\n') advance(c);
        else break;
    }
    Token token = {.line = c->line, .column = c->column};
    if (c->at >= c->length) { token.kind = T_EOF; return token; }
    char ch = peek_char(c, 0);
    if (ch == '\n') { advance(c); token.kind = T_NL; return token; }
    if (digit(ch) || (ch == '.' && digit(peek_char(c, 1)))) {
        /* \d+(\.\d*)?|\.\d+ with an optional exponent; a '.' that starts '..' ends the number. */
        char text[400];
        size_t n = 0;
        while (digit(peek_char(c, 0)) || (peek_char(c, 0) == '.' && peek_char(c, 1) != '.')) {
            if (peek_char(c, 0) == '.' && memchr(text, '.', n)) break;
            if (n + 1 >= sizeof(text)) fail_at(c, token.line, token.column, "number is too long");
            text[n++] = peek_char(c, 0);
            advance(c);
        }
        char e = peek_char(c, 0);
        size_t sign = peek_char(c, 1) == '+' || peek_char(c, 1) == '-';
        if ((e == 'e' || e == 'E') && digit(peek_char(c, 1 + sign))) {
            for (size_t i = 0; i < 1 + sign; ++i) { text[n++] = peek_char(c, 0); advance(c); }
            while (digit(peek_char(c, 0))) {
                if (n + 1 >= sizeof(text)) fail_at(c, token.line, token.column, "number is too long");
                text[n++] = peek_char(c, 0);
                advance(c);
            }
        }
        text[n] = 0;
        token.kind = T_NUMBER;
        token.number = strtod(text, NULL);
        if (!isfinite(token.number)) fail_at(c, token.line, token.column, "number must be finite");
        return token;
    }
    if (letter(ch)) {
        size_t n = 0;
        while (letter(peek_char(c, 0)) || digit(peek_char(c, 0))) {
            if (n + 1 >= NAME) fail_at(c, token.line, token.column, "name exceeds 31 bytes");
            token.text[n++] = peek_char(c, 0);
            advance(c);
        }
        token.kind = T_NAME;
        return token;
    }
    static const char *const pairs[] = {"<=", ">=", "==", "!=", "&&", "||", ".."};
    for (size_t i = 0; i < sizeof(pairs) / sizeof(*pairs); ++i)
        if (ch == pairs[i][0] && peek_char(c, 1) == pairs[i][1]) {
            memcpy(token.text, pairs[i], 3);
            advance(c);
            advance(c);
            token.kind = T_PUNCT;
            return token;
        }
    if (strchr("=+-*/!<>(){}[],.;:", ch)) {
        token.text[0] = ch;
        advance(c);
        token.kind = T_PUNCT;
        return token;
    }
    if ((unsigned char)ch >= 32 && (unsigned char)ch < 127)
        fail_at(c, token.line, token.column, "unexpected character '%c'", ch);
    fail_at(c, token.line, token.column, "unexpected byte 0x%02x", (unsigned char)ch);
}

static Token take(Compiler *c) {
    Token token = c->token;
    if (c->has_ahead) { c->token = c->ahead; c->has_ahead = false; }
    else c->token = scan(c);
    return token;
}

static const Token *lookahead(Compiler *c) {
    if (!c->has_ahead) { c->ahead = scan(c); c->has_ahead = true; }
    return &c->ahead;
}

static bool is(const Compiler *c, const char *punct) {
    return c->token.kind == T_PUNCT && !strcmp(c->token.text, punct);
}

static bool word(const Compiler *c, const char *name) {
    return c->token.kind == T_NAME && !strcmp(c->token.text, name);
}

static Token expect(Compiler *c, const char *punct, const char *what) {
    if (!is(c, punct)) fail_at(c, c->token.line, c->token.column, "expected %s", what);
    return take(c);
}

static Token expect_name(Compiler *c, const char *what) {
    if (c->token.kind != T_NAME) fail_at(c, c->token.line, c->token.column, "expected %s", what);
    return take(c);
}

static void expect_word(Compiler *c, const char *name) {
    if (!word(c, name)) fail_at(c, c->token.line, c->token.column, "expected '%s'", name);
    take(c);
}

static void newlines(Compiler *c) { while (c->token.kind == T_NL) take(c); }
static void separators(Compiler *c) { while (c->token.kind == T_NL || is(c, ";")) take(c); }

static void end(Compiler *c) {
    if (c->token.kind != T_NL && c->token.kind != T_EOF && !is(c, ";") && !is(c, "}"))
        fail_at(c, c->token.line, c->token.column, "expected newline or semicolon");
    separators(c);
}

static bool reserved_name(const char *name) {
    for (size_t i = 0; i < sizeof(reserved) / sizeof(*reserved); ++i)
        if (!strcmp(name, reserved[i])) return true;
    return false;
}

/* ---- Parser --------------------------------------------------------- */

static const struct { const char *text; int precedence, op; } binary[] = {
    {"||", 1, OR}, {"&&", 2, AND}, {"==", 3, EQ}, {"!=", 3, NE}, {"<", 4, LT}, {"<=", 4, LE},
    {">", 4, GT}, {">=", 4, GE}, {"+", 5, ADD}, {"-", 5, SUB}, {"*", 6, MUL}, {"/", 6, DIV},
};

static Expr *node(Compiler *c, ExprKind kind, const Token *at) {
    Expr *e = allocate(c, sizeof(*e));
    e->kind = kind;
    e->line = at->line;
    e->column = at->column;
    return e;
}

/* name[key]: collections are indexed only by the message key. */
static void index_key(Compiler *c) {
    take(c); /* [ */
    if (!word(c, "key"))
        fail_at(c, c->token.line, c->token.column, "a collection is indexed by the message key, as in name[key]");
    take(c);
    expect(c, "]", "']'");
}

static Expr *postfix(Compiler *c, Expr *e) {
    if (is(c, "[")) {
        index_key(c);
        e->kind = E_DOC;
        if (is(c, ".") && lookahead(c)->kind == T_NAME && !strcmp(lookahead(c)->text, "bytes")) {
            take(c);
            take(c);
            e->kind = E_SIZE;
        }
        return e;
    }
    while (is(c, ".")) {
        take(c);
        Token member = expect_name(c, "a field name or bytes");
        if (!strcmp(member.text, "bytes")) {
            Expr *length = node(c, E_LEN, &member);
            length->args[0] = e;
            length->count = 1;
            e = length;
        } else if (e->kind == E_NAME) {
            e->kind = E_FIELD;
            memcpy(e->member, member.text, NAME);
        } else fail_at(c, member.line, member.column, "only bytes can follow a field");
    }
    return e;
}

static Expr *expression(Compiler *c, int minimum, bool multiline) {
    if (++c->depth > DEPTH) fail_at(c, c->token.line, c->token.column, "expression nesting exceeds 64");
    if (multiline) newlines(c);
    Token token = take(c);
    Expr *left;
    if (token.kind == T_NUMBER) {
        left = node(c, E_NUMBER, &token);
        left->number = token.number;
    } else if (token.kind == T_PUNCT && (!strcmp(token.text, "-") || !strcmp(token.text, "!"))) {
        left = node(c, E_UNARY, &token);
        left->op = token.text[0] == '-' ? NEG : NOT;
        left->args[0] = expression(c, 7, multiline);
        left->count = 1;
    } else if (token.kind == T_PUNCT && !strcmp(token.text, "(")) {
        left = expression(c, 0, true);
        newlines(c);
        expect(c, ")", "')'");
    } else if (token.kind == T_NAME) {
        static const struct { const char *name; unsigned count; int op; } calls[] =
            {{"min", 2, MIN}, {"max", 2, MAX}, {"clamp", 3, CLAMP}};
        int call = -1;
        for (int i = 0; i < 3; ++i) if (!strcmp(token.text, calls[i].name)) call = i;
        if (call >= 0 && is(c, "(")) {
            take(c);
            left = node(c, E_CALL, &token);
            left->op = calls[call].op;
            newlines(c);
            while (!is(c, ")")) {
                if (left->count == 3) fail_at(c, token.line, token.column, "%s expects %u arguments", token.text, calls[call].count);
                left->args[left->count++] = expression(c, 0, true);
                newlines(c);
                if (!is(c, ",")) break;
                take(c);
                newlines(c);
            }
            expect(c, ")", "')'");
            if (left->count != calls[call].count)
                fail_at(c, token.line, token.column, "%s expects %u arguments", token.text, calls[call].count);
        } else if (!strcmp(token.text, "dt")) {
            left = node(c, E_DT, &token);
        } else {
            if (reserved_name(token.text)) fail_at(c, token.line, token.column, "expected a number, state, or expression");
            left = node(c, E_NAME, &token);
            memcpy(left->name, token.text, NAME);
            left = postfix(c, left);
        }
    } else fail_at(c, token.line, token.column, "expected a number, state, or expression");
    for (;;) {
        if (multiline) newlines(c);
        int found = -1;
        if (c->token.kind == T_PUNCT)
            for (int i = 0; i < 12; ++i) if (!strcmp(c->token.text, binary[i].text)) found = i;
        if (found < 0 || binary[found].precedence < minimum) break;
        Token op = take(c);
        Expr *e = node(c, E_BINARY, &op);
        e->op = binary[found].op;
        e->args[0] = left;
        e->args[1] = expression(c, binary[found].precedence + 1, multiline);
        e->count = 2;
        left = e;
    }
    --c->depth;
    return left;
}

static Stmt *block(Compiler *c, unsigned depth);

static Stmt *statement(Compiler *c, unsigned depth) {
    Token token = expect_name(c, "a statement or '}'");
    Stmt *s = allocate(c, sizeof(*s));
    s->line = token.line;
    s->column = token.column;
    if (!strcmp(token.text, "if")) {
        s->kind = S_IF;
        s->value = expression(c, 0, false);
        s->then = block(c, depth + 1);
        /* else may follow on the same line or after one newline */
        bool has_else = word(c, "else");
        if (!has_else && c->token.kind == T_NL) {
            const Token *next = lookahead(c);
            if (next->kind == T_NAME && !strcmp(next->text, "else")) { take(c); has_else = true; }
        }
        if (has_else) {
            take(c);
            if (word(c, "if")) s->otherwise = statement(c, depth + 1);
            else s->otherwise = block(c, depth + 1);
        }
        return s;
    }
    if (!strcmp(token.text, "reply") || !strcmp(token.text, "require")) {
        s->kind = token.text[2] == 'p' ? S_REPLY : S_REQUIRE;
        s->value = expression(c, 0, false);
        return s;
    }
    if (reserved_name(token.text)) fail_at(c, token.line, token.column, "expected a statement or '}'");
    memcpy(s->name, token.text, NAME);
    if (is(c, "[")) {
        index_key(c);
        expect(c, ".", "'.replace'");
        Token method = expect_name(c, "replace");
        if (strcmp(method.text, "replace")) fail_at(c, method.line, method.column, "a collection entry supports replace(first .. last, value)");
        expect(c, "(", "'('");
        s->kind = S_REPLACE;
        s->first = expression(c, 0, true);
        newlines(c);
        expect(c, "..", "'..'");
        s->last = expression(c, 0, true);
        newlines(c);
        expect(c, ",", "','");
        s->value = expression(c, 0, true);
        newlines(c);
        expect(c, ")", "')'");
        return s;
    }
    s->kind = S_ASSIGN;
    expect(c, "=", "'='");
    s->value = expression(c, 0, false);
    return s;
}

static Stmt *block(Compiler *c, unsigned depth) {
    if (depth >= DEPTH) fail_at(c, c->token.line, c->token.column, "block nesting exceeds 64");
    newlines(c);
    expect(c, "{", "'{'");
    separators(c);
    Stmt *first = NULL, **tail = &first;
    while (!is(c, "}")) {
        if (c->token.kind == T_EOF) fail_at(c, c->token.line, c->token.column, "expected '}'");
        *tail = statement(c, depth);
        tail = &(*tail)->next;
        end(c);
    }
    take(c);
    return first;
}

static int input_index(const Compiler *c, const char *name) {
    for (unsigned i = 0; i < c->input_count; ++i) if (!strcmp(c->inputs[i].name, name)) return (int)i;
    return -1;
}

static const char *declared_kind(const Compiler *c, const char *name) {
    for (unsigned i = 0; i < c->states; ++i) if (!strcmp(c->state_names[i], name)) return "state";
    for (unsigned i = 0; i < c->collections; ++i) if (!strcmp(c->collection_names[i], name)) return "collection";
    int input = input_index(c, name);
    if (input >= 0) return c->inputs[input].kind == KIND_KEY ? "key input" : "message";
    for (unsigned i = 0; i < c->views; ++i) if (!strcmp(c->view_names[i], name)) return "view";
    return NULL;
}

static void declare(Compiler *c, const Token *name) {
    if (reserved_name(name->text)) fail_at(c, name->line, name->column, "'%s' is reserved", name->text);
    const char *kind = declared_kind(c, name->text);
    if (kind) fail_at(c, name->line, name->column, "'%s' is already declared as a %s", name->text, kind);
}

static bool key_name(const char *key) {
    static const char *const named[] = {"Space", "Enter", "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"};
    for (size_t i = 0; i < sizeof(named) / sizeof(*named); ++i) if (!strcmp(key, named[i])) return true;
    if (!strncmp(key, "Key", 3)) return key[3] >= 'A' && key[3] <= 'Z' && !key[4];
    if (!strncmp(key, "Digit", 5)) return key[5] >= '0' && key[5] <= '9' && !key[6];
    return false;
}

/* { name: u32 ... } with at most one bytes field, which comes last. */
static void message_fields(Compiler *c, Input *in) {
    if (!is(c, "{")) return; /* the field block opens on the declaration's line */
    take(c);
    separators(c);
    while (!is(c, "}")) {
        Token name = expect_name(c, "a field name");
        if (reserved_name(name.text)) fail_at(c, name.line, name.column, "'%s' is reserved", name.text);
        for (unsigned i = 0; i < in->fields; ++i)
            if (!strcmp(in->field_names[i], name.text)) fail_at(c, name.line, name.column, "duplicate field '%s'", name.text);
        if (in->fields && in->field_types[in->fields - 1] == FIELD_BYTES)
            fail_at(c, name.line, name.column, "a bytes field must be the last field");
        if (c->field_count == FIELDS) fail_at(c, name.line, name.column, "at most 32 message fields are allowed");
        expect(c, ":", "':'");
        Token type = expect_name(c, "u32 or bytes");
        if (strcmp(type.text, "u32") && strcmp(type.text, "bytes"))
            fail_at(c, type.line, type.column, "field types are u32 and bytes");
        memcpy(in->field_names[in->fields], name.text, NAME);
        in->field_types[in->fields++] = type.text[0] == 'u' ? FIELD_U32 : FIELD_BYTES;
        ++c->field_count;
        end(c);
    }
    take(c);
}

static void declaration(Compiler *c) {
    Token kind = expect_name(c, "state, input, view, or on");
    if (!strcmp(kind.text, "state")) {
        Token name = expect_name(c, "state name");
        declare(c, &name);
        if (is(c, ":")) {
            take(c);
            expect_word(c, "bytes");
            expect_word(c, "by");
            expect_word(c, "key");
            if (c->collections == COLLECTIONS) fail_at(c, name.line, name.column, "at most one keyed collection is allowed");
            memcpy(c->collection_names[c->collections++], name.text, NAME);
        } else {
            if (c->states == STATES) fail_at(c, name.line, name.column, "at most 32 states are allowed");
            expect(c, "=", "'='");
            double sign = 1;
            if (is(c, "-") || is(c, "+")) sign = take(c).text[0] == '-' ? -1 : 1;
            if (c->token.kind != T_NUMBER) fail_at(c, c->token.line, c->token.column, "expected a numeric state initializer");
            memcpy(c->state_names[c->states], name.text, NAME);
            c->initial[c->states++] = sign * take(c).number;
        }
    } else if (!strcmp(kind.text, "input")) {
        Token name = expect_name(c, "input name");
        declare(c, &name);
        if (c->input_count == INPUTS) fail_at(c, name.line, name.column, "at most 8 inputs are allowed");
        expect(c, "=", "'='");
        Input *in = &c->inputs[c->input_count];
        memset(in, 0, sizeof(*in));
        memcpy(in->name, name.text, NAME);
        if (word(c, "message")) {
            take(c);
            in->kind = KIND_MESSAGE;
            if (word(c, "by")) { take(c); expect_word(c, "key"); in->keyed = true; }
            ++c->input_count;
            message_fields(c, in);
        } else {
            if (!word(c, "key")) fail_at(c, c->token.line, c->token.column, "expected 'key' or 'message'");
            take(c);
            Token key = expect_name(c, "key name");
            if (!key_name(key.text)) fail_at(c, key.line, key.column, "unknown key '%s'", key.text);
            for (unsigned i = 0; i < c->input_count; ++i)
                if (c->inputs[i].kind == KIND_KEY && !strcmp(c->inputs[i].key, key.text))
                    fail_at(c, key.line, key.column, "key %s is already used by input '%s'", key.text, c->inputs[i].name);
            in->kind = KIND_KEY;
            memcpy(in->key, key.text, NAME);
            ++c->input_count;
        }
    } else if (!strcmp(kind.text, "view")) {
        Token name = expect_name(c, "view name");
        declare(c, &name);
        if (c->views == VIEWS) fail_at(c, name.line, name.column, "at most 24 views are allowed");
        expect(c, "=", "'='");
        memcpy(c->view_names[c->views], name.text, NAME);
        c->view_value[c->views++] = expression(c, 0, false);
    } else if (!strcmp(kind.text, "on")) {
        if (c->handler_count == HANDLERS) fail_at(c, kind.line, kind.column, "at most 64 handlers are allowed");
        Token event = expect_name(c, "frame or an input");
        unsigned index = c->handler_count++;
        c->handlers[index].line = event.line;
        c->handlers[index].column = event.column;
        c->handlers[index].edge = -1;
        if (strcmp(event.text, "frame")) {
            /* The input may be declared later; it is resolved at generation. */
            memcpy(c->handlers[index].input, event.text, NAME);
            if (is(c, ".")) {
                take(c);
                Token phase = expect_name(c, "down or up");
                if (strcmp(phase.text, "down") && strcmp(phase.text, "up"))
                    fail_at(c, phase.line, phase.column, "expected down or up");
                c->handlers[index].edge = phase.text[0] == 'u';
            }
        }
        c->handlers[index].body = block(c, 0);
    } else fail_at(c, kind.line, kind.column, "unknown declaration '%s'", kind.text);
    end(c);
}

/* ---- Checks and code generation ------------------------------------- */

static void emit(Compiler *c, int line, int column, uint8_t op, uint32_t arg, double value) {
    if (c->code_count == CODE) fail_at(c, line, column, "program exceeds 512 instructions");
    c->code[c->code_count].op = op;
    c->code[c->code_count].arg = arg;
    c->code[c->code_count++].value = value;
}

static int state_index(const Compiler *c, const char *name) {
    for (unsigned i = 0; i < c->states; ++i) if (!strcmp(c->state_names[i], name)) return (int)i;
    return -1;
}

static const char *type_name(Type type) { return type == NUM ? "a number" : "bytes"; }

/* The keyed collection used by name[key] in the current handler. */
static uint32_t collection(Compiler *c, const char *name, int line, int column) {
    int index = -1;
    for (unsigned i = 0; i < c->collections; ++i) if (!strcmp(c->collection_names[i], name)) index = (int)i;
    if (index < 0) {
        const char *kind = declared_kind(c, name);
        if (kind) fail_at(c, line, column, "'%s' is a %s, not a collection", name, kind);
        fail_at(c, line, column, "unknown collection '%s'", name);
    }
    if (c->message < 0 || !c->inputs[c->message].keyed)
        fail_at(c, line, column, "%s[key] needs the handler of a message by key", name);
    return (uint32_t)index;
}

/* Emit an expression; height is the stack height before it. Returns its type. */
static Type value(Compiler *c, const Expr *e, bool handler, int height) {
    if (height + 1 > STACK) fail_at(c, e->line, e->column, "expression needs more than 64 stack values");
    switch (e->kind) {
    case E_NUMBER: emit(c, e->line, e->column, CONST, 0, e->number); return NUM;
    case E_DT:
        if (!handler) fail_at(c, e->line, e->column, "dt is only available in handlers");
        emit(c, e->line, e->column, DT, 0, 0);
        return NUM;
    case E_NAME: {
        int index = state_index(c, e->name);
        if (index < 0) {
            const char *kind = declared_kind(c, e->name);
            if (kind) fail_at(c, e->line, e->column, "'%s' is a %s, not a state", e->name, kind);
            fail_at(c, e->line, e->column, "unknown state '%s'", e->name);
        }
        emit(c, e->line, e->column, LOAD, (uint32_t)index, 0);
        return NUM;
    }
    case E_FIELD: {
        int input = input_index(c, e->name);
        if (input < 0 || c->inputs[input].kind != KIND_MESSAGE) {
            const char *kind = declared_kind(c, e->name);
            if (kind) fail_at(c, e->line, e->column, "'%s' is a %s and has no fields", e->name, kind);
            fail_at(c, e->line, e->column, "unknown message '%s'", e->name);
        }
        if (input != c->message)
            fail_at(c, e->line, e->column, "fields of '%s' are only available in its handler", e->name);
        const Input *in = &c->inputs[input];
        for (unsigned i = 0; i < in->fields; ++i)
            if (!strcmp(in->field_names[i], e->member)) {
                emit(c, e->line, e->column, FIELD, i, 0);
                return in->field_types[i] == FIELD_U32 ? NUM : BYTES;
            }
        fail_at(c, e->line, e->column, "message '%s' has no field '%s'", e->name, e->member);
    }
    case E_DOC:
        emit(c, e->line, e->column, DOC, collection(c, e->name, e->line, e->column), 0);
        return BYTES;
    case E_SIZE:
        emit(c, e->line, e->column, DOC_SIZE, collection(c, e->name, e->line, e->column), 0);
        return NUM;
    case E_LEN: {
        Type inner = value(c, e->args[0], handler, height);
        if (inner != BYTES) fail_at(c, e->line, e->column, ".bytes needs bytes, not %s", type_name(inner));
        emit(c, e->line, e->column, LEN, 0, 0);
        return NUM;
    }
    default:
        for (unsigned i = 0; i < e->count; ++i) {
            Type operand = value(c, e->args[i], handler, height + (int)i);
            if (operand != NUM)
                fail_at(c, e->args[i]->line, e->args[i]->column, "expected a number, not %s", type_name(operand));
        }
        emit(c, e->line, e->column, (uint8_t)e->op, 0, 0);
        return NUM;
    }
}

static void expect_type(Compiler *c, const Expr *e, Type type, bool handler) {
    Type actual = value(c, e, handler, 0 /* statement operands start at the top */);
    if (actual != type) fail_at(c, e->line, e->column, "expected %s, not %s", type_name(type), type_name(actual));
}

static void statements(Compiler *c, const Stmt *s) {
    for (; s; s = s->next) {
        switch (s->kind) {
        case S_ASSIGN: {
            int index = state_index(c, s->name);
            if (index < 0) {
                const char *kind = declared_kind(c, s->name);
                if (kind) fail_at(c, s->line, s->column, "cannot assign to %s '%s'", kind, s->name);
                fail_at(c, s->line, s->column, "unknown state '%s'", s->name);
            }
            expect_type(c, s->value, NUM, true);
            emit(c, s->line, s->column, STORE, (uint32_t)index, 0);
            break;
        }
        case S_REPLACE: {
            uint32_t target = collection(c, s->name, s->line, s->column);
            expect_type(c, s->first, NUM, true);
            Type last = value(c, s->last, true, 1);
            if (last != NUM) fail_at(c, s->last->line, s->last->column, "expected a number, not bytes");
            Type insert = value(c, s->value, true, 2);
            if (insert != BYTES) fail_at(c, s->value->line, s->value->column, "replace inserts bytes, not a number");
            emit(c, s->line, s->column, REPLACE, target, 0);
            break;
        }
        case S_REPLY:
            if (c->message < 0) fail_at(c, s->line, s->column, "reply is only available in a message handler");
            expect_type(c, s->value, BYTES, true);
            emit(c, s->line, s->column, REPLY, 0, 0);
            break;
        case S_REQUIRE:
            expect_type(c, s->value, NUM, true);
            emit(c, s->line, s->column, REQUIRE, 0, 0);
            break;
        case S_IF: {
            expect_type(c, s->value, NUM, true);
            unsigned jump = c->code_count;
            emit(c, s->line, s->column, JZ, 0, 0);
            statements(c, s->then);
            if (s->otherwise) {
                unsigned skip = c->code_count;
                emit(c, s->line, s->column, JMP, 0, 0);
                c->code[jump].arg = c->code_count;
                statements(c, s->otherwise);
                c->code[skip].arg = c->code_count;
            } else c->code[jump].arg = c->code_count;
            break;
        }
        }
    }
}

static unsigned char *put16(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    return p + 2;
}

static unsigned char *put32(unsigned char *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
    return p + 4;
}

static unsigned char *put_double(unsigned char *p, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put32(p, (uint32_t)bits);
    return put32(p + 4, (uint32_t)(bits >> 32));
}

/* Deduplicated [u8 length][bytes] strings; returns the offset. */
static uint32_t intern(unsigned char *strings, size_t *used, const char *text) {
    size_t length = strlen(text);
    for (size_t at = 0; at < *used; at += 1u + strings[at])
        if (strings[at] == length && !memcmp(strings + at + 1, text, length)) return (uint32_t)at;
    uint32_t offset = (uint32_t)*used;
    strings[(*used)++] = (unsigned char)length;
    memcpy(strings + *used, text, length);
    *used += length;
    return offset;
}

static void generate(Compiler *c, unsigned char **module, size_t *size) {
    uint32_t events[HANDLERS];
    int handler_input[HANDLERS];
    for (unsigned i = 0; i < c->handler_count; ++i) {
        const char *name = c->handlers[i].input;
        int line = c->handlers[i].line, column = c->handlers[i].column;
        events[i] = 0; /* frame */
        handler_input[i] = -1;
        if (!*name) continue;
        int input = input_index(c, name);
        if (input < 0) {
            const char *kind = declared_kind(c, name);
            if (kind) fail_at(c, line, column, "'%s' is a %s, not an input", name, kind);
            fail_at(c, line, column, "unknown input '%s'", name);
        }
        if (c->inputs[input].kind == KIND_MESSAGE && c->handlers[i].edge >= 0)
            fail_at(c, line, column, "'%s' is a message; write on %s { }", name, name);
        if (c->inputs[input].kind == KIND_KEY && c->handlers[i].edge < 0)
            fail_at(c, line, column, "'%s' is a key input; write on %s.down or on %s.up", name, name, name);
        handler_input[i] = input;
        events[i] = 1u + 2u * (uint32_t)input + (c->handlers[i].edge > 0);
    }
    /* Views, then handlers, each as one contiguous block. */
    c->message = -1;
    for (unsigned i = 0; i < c->views; ++i) {
        c->view_code[i][0] = c->code_count;
        Type type = value(c, c->view_value[i], false, 0);
        if (type != NUM) fail_at(c, c->view_value[i]->line, c->view_value[i]->column, "a view is a number");
        c->view_code[i][1] = c->code_count - c->view_code[i][0];
    }
    for (unsigned i = 0; i < c->handler_count; ++i) {
        int input = handler_input[i];
        c->message = input >= 0 && c->inputs[input].kind == KIND_MESSAGE ? input : -1;
        c->handler_code[i][0] = c->code_count;
        statements(c, c->handlers[i].body);
        c->handler_code[i][1] = c->code_count - c->handler_code[i][0];
    }
    static unsigned char strings[(STATES + COLLECTIONS + 2 * INPUTS + FIELDS + VIEWS) * NAME];
    size_t used = 0;
    uint32_t state_name[STATES] = {0}, collection_name[COLLECTIONS] = {0}, input_name[INPUTS] = {0}, input_key[INPUTS] = {0};
    uint32_t field_name[FIELDS] = {0}, view_name[VIEWS] = {0};
    unsigned fields = 0;
    for (unsigned i = 0; i < c->states; ++i) state_name[i] = intern(strings, &used, c->state_names[i]);
    for (unsigned i = 0; i < c->collections; ++i) collection_name[i] = intern(strings, &used, c->collection_names[i]);
    for (unsigned i = 0; i < c->input_count; ++i) {
        input_name[i] = intern(strings, &used, c->inputs[i].name);
        input_key[i] = c->inputs[i].kind == KIND_KEY ? intern(strings, &used, c->inputs[i].key) : 0;
        for (unsigned f = 0; f < c->inputs[i].fields; ++f)
            field_name[fields++] = intern(strings, &used, c->inputs[i].field_names[f]);
    }
    for (unsigned i = 0; i < c->views; ++i) view_name[i] = intern(strings, &used, c->view_names[i]);
    *size = 32 + 16u * c->states + 12u * c->collections + 16u * c->input_count + 8u * fields +
            12u * (c->views + c->handler_count) + 16u * c->code_count + used;
    unsigned char *p = *module = allocate(c, *size);
    memcpy(p, "PUREMOD2", 8);
    put16(p + 8, c->states);
    put16(p + 10, c->input_count);
    put16(p + 12, c->views);
    put16(p + 14, c->handler_count);
    put32(p + 16, c->code_count);
    put32(p + 20, (uint32_t)used);
    put16(p + 24, fields);
    put16(p + 26, c->collections);
    p += 32;
    for (unsigned i = 0; i < c->states; ++i) { p = put32(p, state_name[i]); p = put32(p, 0); p = put_double(p, c->initial[i]); }
    for (unsigned i = 0; i < c->collections; ++i) { p = put32(p, collection_name[i]); p = put32(p, 1); p = put32(p, 1); }
    unsigned first = 0;
    for (unsigned i = 0; i < c->input_count; ++i) {
        const Input *in = &c->inputs[i];
        p = put32(p, input_name[i]);
        *p++ = (unsigned char)in->kind;
        *p++ = in->keyed;
        p = put16(p, 0);
        p = put32(p, input_key[i]);
        p = put16(p, in->kind == KIND_MESSAGE ? first : 0);
        p = put16(p, in->fields);
        first += in->fields;
    }
    fields = 0;
    for (unsigned i = 0; i < c->input_count; ++i)
        for (unsigned f = 0; f < c->inputs[i].fields; ++f) {
            p = put32(p, field_name[fields++]);
            p = put32(p, (uint32_t)c->inputs[i].field_types[f]);
        }
    for (unsigned i = 0; i < c->views; ++i) { p = put32(p, view_name[i]); p = put32(p, c->view_code[i][0]); p = put32(p, c->view_code[i][1]); }
    for (unsigned i = 0; i < c->handler_count; ++i) {
        p = put32(p, events[i]);
        p = put32(p, c->handler_code[i][0]);
        p = put32(p, c->handler_code[i][1]);
    }
    for (unsigned i = 0; i < c->code_count; ++i) {
        *p++ = c->code[i].op; *p++ = 0; *p++ = 0; *p++ = 0;
        p = put32(p, c->code[i].arg);
        p = put_double(p, c->code[i].value);
    }
    memcpy(p, strings, used);
}

bool aup_compile(const char *source, size_t length, unsigned char **module, size_t *size,
                 AupDiagnostic *error) {
    *module = NULL;
    *size = 0;
    *error = (AupDiagnostic){0};
    Compiler *c = calloc(1, sizeof(*c));
    if (!c) { *error = (AupDiagnostic){1, 1, "out of memory"}; return false; }
    c->source = source;
    c->length = length;
    c->line = c->column = 1;
    c->error = error;
    unsigned char *result = NULL;
    if (!setjmp(c->fail)) {
        if (length > AUP_SOURCE_LIMIT) fail_at(c, 1, 1, "source exceeds 32768 bytes");
        c->token = scan(c);
        separators(c);
        while (c->token.kind != T_EOF) declaration(c);
        generate(c, &result, size);
        *module = malloc(*size);
        if (!*module) fail_at(c, 1, 1, "out of memory");
        memcpy(*module, result, *size);
    } else {
        free(*module);
        *module = NULL;
        *size = 0;
    }
    for (size_t i = 0; i < c->block_count; ++i) free(c->blocks[i]);
    free(c->blocks);
    free(c);
    return *module != NULL;
}
