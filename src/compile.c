#include "aup.h"
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Limits and opcodes of Pure module format v5. */
enum { STATES = 32, INPUTS = 32, VIEWS = 64, HANDLERS = 64, CODE = 65535, STACK = 64, NAME = 32, JOINS = 8,
       DEPTH = 64, FIELDS = 32, COLLECTIONS = 1, CONSTANTS = 4096, CONSTANT_BYTES = 1 << 20,
       RECORDS = 8, RECORD_FIELDS = 64, FIELDS_PER_RECORD = 16, FNS = 64, PARAMS = 8, MODULES = 9 };
enum { CONST, LOAD, DT, ADD, SUB, MUL, DIV, NEG, LT, LE, GT, GE, EQ, NE, AND, OR, NOT,
       MIN, MAX, CLAMP, STORE, JZ, JMP, FIELD, DOC, DOC_SIZE, LEN, REPLACE, REPLY, REQUIRE,
       TEXT_CONST, TEXT_LOAD, TEXT_INPUT, NUM_TEXT, CONCAT, CHARS, LINES, TEXT_BYTES,
       CUR_LOAD, CUR_START, CUR_END, CUR_MOVE, CUR_STORE, TEXT_INSERT, TEXT_REMOVE, TEXT_LINE,
       TEXT_SET, TEXT_FROM, BYTES_FROM, CUR_COUNT, LIST_COUNT, LIST_APPEND, LIST_INSERT, LIST_REMOVE,
       ITEM_GET, ITEM_SET, INDEX_LOAD, INDEX_STORE, INDEX_MOVE, INDEX_FIRST, INDEX_LAST, INDEX_NUM, ROW,
       PICK, SLIDE, FOLD, TEXT_COMPARE, LIST_FIND, LIST_HAS, NOW, FLOOR, MOD };
enum { FOLD_COUNT, FOLD_SUM, FOLD_MAX, FOLD_MIN, FOLD_FIRST };
enum { KIND_KEY = 1, KIND_MESSAGE = 2, KIND_TEXT = 3, KIND_TIMER = 4, FIELD_U32 = 1, FIELD_BYTES = 2, FIELD_NUMBER = 3, FIELD_TEXT = 4 };
enum { STATE_NUM = 0, STATE_TEXT = 1, STATE_CURSOR = 2, STATE_LIST = 3, STATE_INDEX = 4, UNIT_CHAR = 0, UNIT_LINE = 1 };
typedef enum { NUM = 1, BYTES, TEXT, CURSOR, UNIT, INDEX } Type;
typedef struct { Type type; int owner; } Kind; /* owner: a cursor's text state or an index's list state */

typedef enum { T_EOF, T_NL, T_NUMBER, T_NAME, T_PUNCT, T_STRING } TokenKind;
typedef struct {
    TokenKind kind;
    int line, column;
    double number;
    char text[NAME + 1]; /* name, or punctuation of one or two characters */
    size_t start, end;   /* string contents in the source */
} Token;

typedef enum {
    E_NUMBER, E_NAME, E_DT, E_NOW, E_UNARY, E_BINARY, E_CALL,
    E_MEMBER,  /* args[0].member */
    E_DOC,     /* name[key] */
    E_SIZE,    /* name[key].bytes */
    E_STRING,  /* parts linked by next: E_TEXT literals and expressions */
    E_TEXT,    /* literal bytes */
    E_UNIT,    /* args[0] char|line */
    E_CONVERT, /* text(args[0]) or bytes(args[0]) */
    E_FN,      /* name(args): a fn, qualified by the library in member (name.line(at) when member is a text) */
    E_ITEM,    /* name[args[0]].member: a field of a list item */
    E_RECORD,  /* name { fields }: E_FIELD nodes linked from args[0] */
    E_FIELD,   /* member: args[0] */
    E_IF,      /* if args[0] { args[1] } else { args[2] } */
    E_FOLD,    /* op(args[1] for name in member where args[0]): count, first (no value), sum, max, min */
} ExprKind;
typedef struct Expr {
    ExprKind kind;
    int line, column, op;
    double number;
    char name[NAME], member[NAME];
    struct Expr *args[PARAMS], *next;
    unsigned count;
    unsigned char *bytes;
    size_t size;
} Expr;

typedef enum { S_ASSIGN, S_IF, S_REPLACE, S_REPLY, S_REQUIRE, S_INSERT, S_REMOVE, S_APPEND, S_ADD, S_SET } StmtKind;
typedef struct Stmt {
    StmtKind kind;
    int line, column;
    char name[NAME], member[NAME]; /* member: the item field an S_SET changes */
    Expr *value;          /* assigned value, condition, reply, requirement or insertion */
    Expr *first, *last;   /* a range, the insertion cursor or index, or the item index; last may be NULL */
                          /* S_SET: name is the list, or empty when first is an index naming the item */
    struct Stmt *then, *otherwise, *next;
} Stmt;

typedef struct {
    char name[NAME], key[NAME];
    int kind;
    uint32_t period; /* a timer's, in milliseconds */
    bool keyed;
    char field_names[FIELDS][NAME];
    int field_types[FIELDS];
    unsigned fields;
} Input;

/* A record: fields in order, each a number or text with its default. */
typedef struct {
    char name[NAME];
    int line, column;
    char field_names[FIELDS_PER_RECORD][NAME];
    bool field_text[FIELDS_PER_RECORD];
    double field_number[FIELDS_PER_RECORD];
    Expr *field_string[FIELDS_PER_RECORD]; /* a text default, or NULL for "" */
    uint32_t field_constant[FIELDS_PER_RECORD];
    unsigned fields;
} Record;

/* A fn: one expression over its parameters, inlined where it is called. */
typedef struct {
    char name[NAME], params[PARAMS][NAME];
    unsigned param_count;
    int module, line, column;
    Expr *body;
    bool active; /* being inlined: a call back into it is recursion */
} Fn;

/* The fn being inlined: where its arguments are on the stack and their kinds. */
typedef struct Scope {
    int fn, base, line, column;
    Kind kinds[PARAMS];
} Scope;

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
    char state_names[STATES][NAME], state_target[STATES][NAME];
    int state_types[STATES], state_line[STATES], state_column[STATES];
    bool state_stays[STATES]; /* a mark: a cursor that stays before text inserted at it */
    /* A keyed list: the field named after by, and once resolved, that field + 1. */
    char state_key[STATES][NAME];
    int state_key_line[STATES], state_key_column[STATES], list_key[STATES];
    double initial[STATES];
    Expr *state_text[STATES];
    unsigned states;
    char collection_names[COLLECTIONS][NAME];
    unsigned collections;
    Record records[RECORDS];
    unsigned record_count, record_fields;
    Input inputs[INPUTS];
    unsigned input_count, field_count;
    /* Views in order; a table (no value) is followed by its columns. */
    char view_names[VIEWS][NAME], view_row[VIEWS][NAME], view_list[VIEWS][NAME];
    Expr *view_value[VIEWS];
    Expr *view_filter[VIEWS];                         /* a table's where condition, or NULL */
    unsigned char view_byte[VIEWS];                   /* a filtered table's list + 1 */
    int view_table[VIEWS];                            /* a column's table + 1, or 0 */
    int view_line[VIEWS][2], view_column[VIEWS][2];   /* a table's row name and list name */
    unsigned views;
    struct { char input[NAME]; int edge, line, column; Stmt *body; } handlers[HANDLERS];
    unsigned handler_count;
    /* Code generation. */
    int message;          /* message or text input of the handler being generated, or -1 */
    int row;              /* the table whose column is being generated, or -1 */
    /* The fold whose block is being generated: its item's name and list, and
     * the fn scope it began in (whose parameters its block cannot read). */
    bool folding, filtering;
    char fold_name[NAME];
    int fold_list;
    Scope *fold_scope;
    /* Libraries: module 0 is the main source; each module lists the libraries it uses. */
    struct { char name[NAME], path[AUP_PATH]; bool uses[MODULES], parsing; } modules[MODULES];
    unsigned module_count;
    int module;           /* the module being parsed, or whose fn is being inlined */
    AupLoad load;
    void *load_context;
    Fn fns[FNS];
    unsigned fn_count;
    Scope *scope;         /* the fn being inlined, or NULL */
    unsigned joins;       /* branches ahead that carry a value */
    struct { uint8_t op; uint32_t arg; double value; } code[CODE];
    unsigned code_count;
    uint32_t view_code[VIEWS][2], handler_code[HANDLERS][2];
    int view_types[VIEWS];
    unsigned char constant_data[CONSTANT_BYTES];
    uint32_t constant_offset[CONSTANTS], constant_length[CONSTANTS];
    unsigned constants;
    size_t constant_used;
} Compiler;

static const char *const reserved[] = {"state", "input", "view", "on", "if", "else", "key", "frame",
                                       "dt", "min", "max", "clamp", "message", "by", "bytes", "u32",
                                       "reply", "require", "record", "each", "fn", "use", "now", "every",
                                       "floor"};

/* An error in the module being parsed or inlined; inside a fn it names the call. */
static _Noreturn void fail_at(Compiler *c, int line, int column, const char *format, ...) {
    va_list args;
    va_start(args, format);
    c->error->line = line;
    c->error->column = column;
    snprintf(c->error->path, sizeof(c->error->path), "%s", c->modules[c->module].path);
    int used = vsnprintf(c->error->message, sizeof(c->error->message), format, args);
    va_end(args);
    if (c->scope && used >= 0 && (size_t)used < sizeof(c->error->message))
        snprintf(c->error->message + used, sizeof(c->error->message) - (size_t)used, " (in fn '%s' called at %d:%d)",
                 c->fns[c->scope->fn].name, c->scope->line, c->scope->column);
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
    void *block = calloc(1, size ? size : 1);
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
    if (ch == '"') {
        /* One line; escapes and {expressions} are read by the parser. */
        advance(c);
        token.kind = T_STRING;
        token.start = c->at;
        for (;;) {
            char next = peek_char(c, 0);
            if (c->at >= c->length || next == '\n' || next == '\r')
                fail_at(c, token.line, token.column, "a string ends on the line it starts");
            if (next == '"') break;
            if (next == '\\' && c->at + 1 < c->length) advance(c);
            advance(c);
        }
        token.end = c->at;
        advance(c);
        return token;
    }
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
    if (strchr("=+-*/%!<>(){}[],.;:", ch)) {
        token.text[0] = ch;
        advance(c);
        token.kind = T_PUNCT;
        return token;
    }
    if ((unsigned char)ch >= 32 && (unsigned char)ch < 127)
        fail_at(c, token.line, token.column, "unexpected character '%c'", ch);
    fail_at(c, token.line, token.column, "unexpected byte 0x%02x outside a string", (unsigned char)ch);
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

/* Strict UTF-8, as Pure checks its constants. */
static bool utf8_valid(const unsigned char *bytes, size_t size) {
    for (size_t i = 0; i < size;) {
        unsigned char b = bytes[i];
        size_t need = b < 0x80 ? 0 : b >= 0xC2 && b <= 0xDF ? 1 : b >= 0xE0 && b <= 0xEF ? 2 :
                      b >= 0xF0 && b <= 0xF4 ? 3 : 4;
        if (need == 4 || need > size - i - 1) return false;
        unsigned low = b == 0xE0 ? 0xA0 : b == 0xF0 ? 0x90 : 0x80, high = b == 0xED ? 0x9F : b == 0xF4 ? 0x8F : 0xBF;
        for (size_t k = 1; k <= need; ++k) {
            unsigned char next = bytes[i + k];
            if (next < (k == 1 ? low : 0x80) || next > (k == 1 ? high : 0xBF)) return false;
        }
        i += need + 1;
    }
    return true;
}

/* ---- Parser --------------------------------------------------------- */

static const struct { const char *text; int precedence, op; } binary[] = {
    {"||", 1, OR}, {"&&", 2, AND}, {"==", 3, EQ}, {"!=", 3, NE}, {"<", 4, LT}, {"<=", 4, LE},
    {">", 4, GT}, {">=", 4, GE}, {"+", 5, ADD}, {"-", 5, SUB}, {"*", 6, MUL}, {"/", 6, DIV}, {"%", 6, MOD},
};

static Expr *node(Compiler *c, ExprKind kind, int line, int column) {
    Expr *e = allocate(c, sizeof(*e));
    e->kind = kind;
    e->line = line;
    e->column = column;
    return e;
}

static Expr *expression(Compiler *c, int minimum, bool multiline);

/* name[key]: collections are indexed only by the message key. */
static void index_key(Compiler *c) {
    take(c); /* [ */
    if (!word(c, "key"))
        fail_at(c, c->token.line, c->token.column, "a collection is indexed by the message key, as in name[key]");
    take(c);
    expect(c, "]", "']'");
}

/* (a, b, ...) after a fn name. */
/* Arguments once the opening parenthesis is taken. */
static Expr *arguments_after(Compiler *c, Expr *call) {
    newlines(c);
    while (!is(c, ")")) {
        if (call->count == PARAMS) fail_at(c, c->token.line, c->token.column, "a call takes at most 8 arguments");
        call->args[call->count++] = expression(c, 0, true);
        newlines(c);
        if (!is(c, ",")) break;
        take(c);
        newlines(c);
    }
    expect(c, ")", "')'");
    return call;
}

static Expr *arguments(Compiler *c, Expr *call) {
    expect(c, "(", "'('");
    return arguments_after(c, call);
}

static Expr *postfix(Compiler *c, Expr *e) {
    const Token *next = is(c, "[") ? lookahead(c) : NULL;
    if (e->kind == E_NAME && next && next->kind == T_NAME && !strcmp(next->text, "key")) {
        index_key(c);
        e->kind = E_DOC;
        if (is(c, ".") && lookahead(c)->kind == T_NAME && !strcmp(lookahead(c)->text, "bytes")) {
            take(c);
            take(c);
            e->kind = E_SIZE;
        }
        return e;
    }
    if (e->kind == E_NAME && next) {
        /* tasks[at].title: a field of a list item. */
        take(c);
        Expr *item = node(c, E_ITEM, e->line, e->column);
        memcpy(item->name, e->name, NAME);
        item->args[0] = expression(c, 0, true);
        item->count = 1;
        newlines(c);
        expect(c, "]", "']'");
        if (is(c, ".")) {
            take(c);
            memcpy(item->member, expect_name(c, "a field name").text, NAME);
        }
        e = item;
    }
    while (is(c, ".")) {
        take(c);
        Token member = expect_name(c, "a field or member name");
        if (is(c, "(")) {
            /* library.fn(...), or text.line(cursor) */
            if (e->kind != E_NAME)
                fail_at(c, member.line, member.column, "only a library fn or a text's line is called here");
            Expr *call = node(c, E_FN, member.line, member.column);
            memcpy(call->name, member.text, NAME);
            memcpy(call->member, e->name, NAME);
            e = arguments(c, call);
            continue;
        }
        Expr *access = node(c, E_MEMBER, e->line, e->column); /* errors point at the expression start */
        access->args[0] = e;
        access->count = 1;
        memcpy(access->member, member.text, NAME);
        e = access;
    }
    return e;
}

/* "text {expression} more": literal parts and expressions, escapes decoded. */
static Expr *string(Compiler *c, const Token *token) {
    Expr *s = node(c, E_STRING, token->line, token->column), **tail = &s->args[0];
    unsigned char *literal = allocate(c, token->end - token->start + 1);
    size_t used = 0, at = token->start;
    int literal_column = token->column;
    while (at <= token->end) {
        char ch = at < token->end ? c->source[at] : 0;
        int column = token->column + 1 + (int)(at - token->start);
        if (at == token->end || ch == '{') {
            if (used || (at == token->end && !s->args[0])) {
                if (!utf8_valid(literal, used)) fail_at(c, token->line, literal_column, "a string must be valid UTF-8");
                Expr *part = node(c, E_TEXT, token->line, literal_column);
                part->bytes = allocate(c, used);
                if (used) memcpy(part->bytes, literal, used);
                part->size = used;
                *tail = part;
                tail = &part->next;
                used = 0;
            }
            if (at == token->end) break;
            size_t close = at + 1;
            while (close < token->end && c->source[close] != '}') ++close;
            if (close == token->end) fail_at(c, token->line, column, "expected '}' to end the {expression}");
            /* Parse the expression with a scanner limited to its braces. */
            size_t saved_at = c->at, saved_length = c->length;
            int saved_line = c->line, saved_column = c->column;
            Token saved_token = c->token, saved_ahead = c->ahead;
            bool saved_has_ahead = c->has_ahead;
            c->at = at + 1;
            c->length = close;
            c->line = token->line;
            c->column = column + 1;
            c->has_ahead = false;
            c->token = scan(c);
            if (c->token.kind == T_EOF) fail_at(c, token->line, column, "empty {expression}");
            Expr *part = expression(c, 0, false);
            if (c->token.kind != T_EOF) fail_at(c, c->token.line, c->token.column, "expected '}'");
            c->at = saved_at;
            c->length = saved_length;
            c->line = saved_line;
            c->column = saved_column;
            c->token = saved_token;
            c->ahead = saved_ahead;
            c->has_ahead = saved_has_ahead;
            *tail = part;
            tail = &part->next;
            at = close + 1;
            literal_column = token->column + 1 + (int)(at - token->start);
            continue;
        }
        if (ch == '\\') {
            char escaped = c->source[at + 1];
            const char *simple = "n\nt\tr\r\"\"\\\\{{}}";
            const char *found = strchr(simple, escaped);
            if (escaped && found && (found - simple) % 2 == 0) {
                literal[used++] = (unsigned char)found[1];
                at += 2;
                continue;
            }
            if (escaped == 'u' && c->source[at + 2] == '{') {
                size_t close = at + 3;
                unsigned long value = 0;
                while (close < token->end && c->source[close] != '}' && close - at - 3 < 6) {
                    char h = c->source[close];
                    int digit_value = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 :
                                      h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (digit_value < 0) break;
                    value = value * 16 + (unsigned long)digit_value;
                    ++close;
                }
                if (close == at + 3 || close >= token->end || c->source[close] != '}' ||
                    value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
                    fail_at(c, token->line, column, "\\u{...} needs 1-6 hex digits naming a Unicode scalar");
                if (value < 0x80) literal[used++] = (unsigned char)value;
                else if (value < 0x800) {
                    literal[used++] = (unsigned char)(0xC0 | value >> 6);
                    literal[used++] = (unsigned char)(0x80 | (value & 0x3F));
                } else if (value < 0x10000) {
                    literal[used++] = (unsigned char)(0xE0 | value >> 12);
                    literal[used++] = (unsigned char)(0x80 | (value >> 6 & 0x3F));
                    literal[used++] = (unsigned char)(0x80 | (value & 0x3F));
                } else {
                    literal[used++] = (unsigned char)(0xF0 | value >> 18);
                    literal[used++] = (unsigned char)(0x80 | (value >> 12 & 0x3F));
                    literal[used++] = (unsigned char)(0x80 | (value >> 6 & 0x3F));
                    literal[used++] = (unsigned char)(0x80 | (value & 0x3F));
                }
                at = close + 1;
                continue;
            }
            fail_at(c, token->line, column, "unknown escape; use \\n \\t \\r \\\" \\\\ \\{ \\} or \\u{...}");
        }
        if (ch == '}') fail_at(c, token->line, column, "a '}' in a string is written \\}");
        literal[used++] = (unsigned char)ch;
        ++at;
    }
    return s;
}

/* { value } of a conditional expression. */
static Expr *branch_value(Compiler *c) {
    newlines(c);
    expect(c, "{", "'{'");
    Expr *value = expression(c, 0, true);
    newlines(c);
    expect(c, "}", "'}'");
    return value;
}

/* if condition { value } else { value }, with else if chains. */
static Expr *conditional(Compiler *c, const Token *token) {
    if (++c->depth > DEPTH) fail_at(c, token->line, token->column, "expression nesting exceeds 64");
    Expr *e = node(c, E_IF, token->line, token->column);
    e->args[0] = expression(c, 0, false);
    e->args[1] = branch_value(c);
    if (c->token.kind == T_NL && lookahead(c)->kind == T_NAME && !strcmp(lookahead(c)->text, "else")) take(c);
    if (!word(c, "else")) fail_at(c, c->token.line, c->token.column, "an if expression needs else { value }");
    take(c);
    if (word(c, "if")) {
        Token next = take(c);
        e->args[2] = conditional(c, &next);
    } else e->args[2] = branch_value(c);
    e->count = 3;
    --c->depth;
    return e;
}

/* The rest of a fold, after its value (none for count): for item in list,
 * then an optional where condition, then ')'. */
static Expr *fold(Compiler *c, const Token *name, int how, Expr *value) {
    Expr *e = node(c, E_FOLD, name->line, name->column);
    e->op = how;
    Token item = expect_name(c, "a name for each item");
    memcpy(e->name, item.text, NAME);
    newlines(c);
    if (!word(c, "in")) fail_at(c, c->token.line, c->token.column, "expected 'in' and a list, as in %s(%s in tasks)",
                                name->text, item.text);
    take(c);
    Token list = expect_name(c, "a list");
    memcpy(e->member, list.text, NAME);
    newlines(c);
    e->args[1] = value;
    if (word(c, "where")) {
        take(c);
        e->args[0] = expression(c, 0, true);
        newlines(c);
    }
    expect(c, ")", "')'");
    return e;
}

static Expr *expression(Compiler *c, int minimum, bool multiline) {
    if (++c->depth > DEPTH) fail_at(c, c->token.line, c->token.column, "expression nesting exceeds 64");
    if (multiline) newlines(c);
    Token token = take(c);
    Expr *left;
    if (token.kind == T_NUMBER) {
        left = node(c, E_NUMBER, token.line, token.column);
        left->number = token.number;
    } else if (token.kind == T_STRING) {
        left = string(c, &token);
    } else if (token.kind == T_PUNCT && (!strcmp(token.text, "-") || !strcmp(token.text, "!"))) {
        left = node(c, E_UNARY, token.line, token.column);
        left->op = token.text[0] == '-' ? NEG : NOT;
        left->args[0] = expression(c, 7, multiline);
        left->count = 1;
    } else if (token.kind == T_PUNCT && !strcmp(token.text, "(")) {
        left = expression(c, 0, true);
        newlines(c);
        expect(c, ")", "')'");
    } else if (token.kind == T_NAME) {
        static const struct { const char *name; unsigned count; int op; } calls[] =
            {{"min", 2, MIN}, {"max", 2, MAX}, {"clamp", 3, CLAMP}, {"text", 1, TEXT_FROM}, {"bytes", 1, BYTES_FROM},
             {"floor", 1, FLOOR}};
        int call = -1, how = -1;
        for (int i = 0; i < (int)(sizeof(calls) / sizeof(*calls)); ++i) if (!strcmp(token.text, calls[i].name)) call = i;
        static const char *const folds[] = {"count", "sum", "max", "min", "first"};
        for (int i = 0; i < 5; ++i) if (!strcmp(token.text, folds[i])) how = i;
        Expr *first = NULL;
        /* count(t in tasks ...), first(t in tasks ...), sum(t.x for t in tasks ...):
         * folds; max(a, b) stays a call. */
        bool itemwise = how == FOLD_COUNT || how == FOLD_FIRST;
        if (how >= 0 && is(c, "(")) {
            take(c);
            newlines(c);
            if (itemwise && c->token.kind == T_NAME && lookahead(c)->kind == T_NAME && !strcmp(lookahead(c)->text, "in")) {
                left = postfix(c, fold(c, &token, how, NULL)); /* first(...).title */
                goto parsed;
            }
            if (!itemwise) {
                first = expression(c, 0, true);
                newlines(c);
                if (word(c, "for")) {
                    take(c);
                    left = postfix(c, fold(c, &token, how, first));
                    goto parsed;
                }
            }
            if (call < 0) {
                /* A fn of that name. */
                if (reserved_name(token.text)) fail_at(c, token.line, token.column, "expected a number, state, or expression");
                left = node(c, E_FN, token.line, token.column);
                memcpy(left->name, token.text, NAME);
                if (first) {
                    left->args[left->count++] = first;
                    if (is(c, ",")) take(c);
                    else if (!is(c, ")")) fail_at(c, c->token.line, c->token.column, "expected ',' or ')'");
                }
                left = postfix(c, arguments_after(c, left));
                goto parsed;
            }
        }
        if (call >= 0 && (first || is(c, "("))) {
            if (!first) take(c);
            left = node(c, calls[call].op == TEXT_FROM || calls[call].op == BYTES_FROM ? E_CONVERT : E_CALL, token.line, token.column);
            left->op = calls[call].op;
            newlines(c);
            if (first) {
                left->args[left->count++] = first;
                if (is(c, ",")) { take(c); newlines(c); }
                else if (!is(c, ")")) fail_at(c, c->token.line, c->token.column, "expected ',' or ')'");
            }
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
                fail_at(c, token.line, token.column, "%s expects %u argument%s", token.text, calls[call].count,
                        calls[call].count == 1 ? "" : "s");
        } else if (!strcmp(token.text, "dt") || !strcmp(token.text, "now")) {
            left = node(c, token.text[0] == 'd' ? E_DT : E_NOW, token.line, token.column);
        } else if (!strcmp(token.text, "if")) {
            left = conditional(c, &token);
        } else {
            if (reserved_name(token.text)) fail_at(c, token.line, token.column, "expected a number, state, or expression");
            left = node(c, is(c, "(") ? E_FN : E_NAME, token.line, token.column);
            memcpy(left->name, token.text, NAME);
            if (left->kind == E_FN) left = arguments(c, left);
            left = postfix(c, left);
        }
    } else fail_at(c, token.line, token.column, "expected a number, state, or expression");
parsed:
    /* 1 char, n line: a distance that moves a cursor. */
    if (word(c, "char") || word(c, "line")) {
        Token unit = take(c);
        Expr *distance = node(c, E_UNIT, unit.line, unit.column);
        distance->op = unit.text[0] == 'c' ? UNIT_CHAR : UNIT_LINE;
        distance->args[0] = left;
        distance->count = 1;
        left = distance;
    }
    for (;;) {
        if (multiline) newlines(c);
        int found = -1;
        if (c->token.kind == T_PUNCT)
            for (int i = 0; i < (int)(sizeof(binary) / sizeof(*binary)); ++i) if (!strcmp(c->token.text, binary[i].text)) found = i;
        if (found < 0 || binary[found].precedence < minimum) break;
        Token op = take(c);
        Expr *e = node(c, E_BINARY, op.line, op.column);
        e->op = binary[found].op;
        e->args[0] = left;
        e->args[1] = expression(c, binary[found].precedence + 1, multiline);
        e->count = 2;
        left = e;
    }
    --c->depth;
    return left;
}

/* Record { field: value, ... }: fields end at a comma, semicolon or line end. */
static Expr *record_value(Compiler *c) {
    Token name = take(c);
    take(c); /* { */
    Expr *r = node(c, E_RECORD, name.line, name.column), **tail = &r->args[0];
    memcpy(r->name, name.text, NAME);
    separators(c);
    while (!is(c, "}")) {
        Token field = expect_name(c, "a field name");
        expect(c, ":", "':'");
        Expr *f = node(c, E_FIELD, field.line, field.column);
        memcpy(f->member, field.text, NAME);
        f->args[0] = expression(c, 0, false);
        f->count = 1;
        *tail = f;
        tail = &f->next;
        if (!is(c, ",") && !is(c, ";") && !is(c, "}") && c->token.kind != T_NL)
            fail_at(c, c->token.line, c->token.column, "expected ',' or a new line between fields");
        while (is(c, ",") || is(c, ";") || c->token.kind == T_NL) take(c);
    }
    take(c);
    return r;
}

/* A value inserted into a text or a list: an expression or a record. */
static Expr *inserted(Compiler *c) {
    newlines(c);
    if (c->token.kind == T_NAME && lookahead(c)->kind == T_PUNCT && !strcmp(lookahead(c)->text, "{"))
        return record_value(c);
    return expression(c, 0, true);
}

static Stmt *block(Compiler *c, unsigned depth);

static void range(Compiler *c, Stmt *s) {
    s->first = expression(c, 0, true);
    newlines(c);
    expect(c, "..", "'..'");
    s->last = expression(c, 0, true);
    newlines(c);
}

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
    if (is(c, "[") && !(lookahead(c)->kind == T_NAME && !strcmp(lookahead(c)->text, "key"))) {
        /* tasks[at].field = value */
        take(c);
        s->kind = S_SET;
        s->first = expression(c, 0, true);
        newlines(c);
        expect(c, "]", "']'");
        if (is(c, ".") && lookahead(c)->kind == T_NAME) {
            take(c);
            Token field = take(c);
            if (is(c, "("))
                fail_at(c, s->first->line, s->first->column, "a collection is indexed by the message key, as in name[key]");
            memcpy(s->member, field.text, NAME);
        } else fail_at(c, c->token.line, c->token.column, "expected '.' and the field to change, as in %s[at].field = value",
                       s->name);
        expect(c, "=", "'='");
        s->value = expression(c, 0, false);
        return s;
    }
    if (is(c, "[")) {
        index_key(c);
        expect(c, ".", "'.replace'");
        Token method = expect_name(c, "replace");
        if (strcmp(method.text, "replace")) fail_at(c, method.line, method.column, "a collection entry supports replace(first .. last, value)");
        expect(c, "(", "'('");
        s->kind = S_REPLACE;
        range(c, s);
        expect(c, ",", "','");
        s->value = expression(c, 0, true);
        newlines(c);
        expect(c, ")", "')'");
        return s;
    }
    if (is(c, ".")) {
        take(c);
        Token method = expect_name(c, "insert, remove, append or a field");
        if (is(c, "=")) {
            /* at.field = value: a field of the item an index names. */
            take(c);
            s->kind = S_SET;
            s->first = node(c, E_NAME, token.line, token.column);
            memcpy(s->first->name, s->name, NAME);
            s->name[0] = 0;
            memcpy(s->member, method.text, NAME);
            s->value = expression(c, 0, false);
            return s;
        }
        expect(c, "(", "'('");
        if (!strcmp(method.text, "insert")) {
            s->kind = S_INSERT;
            s->first = expression(c, 0, true);
            newlines(c);
            expect(c, ",", "','");
            s->value = inserted(c);
            newlines(c);
        } else if (!strcmp(method.text, "remove")) {
            /* A text removes a range; a list removes the item at an index. */
            s->kind = S_REMOVE;
            s->first = expression(c, 0, true);
            newlines(c);
            if (is(c, "..")) {
                take(c);
                s->last = expression(c, 0, true);
                newlines(c);
            }
        } else if (!strcmp(method.text, "append") || !strcmp(method.text, "add")) {
            s->kind = method.text[1] == 'p' ? S_APPEND : S_ADD;
            s->value = inserted(c);
            newlines(c);
        } else fail_at(c, method.line, method.column,
                       "a text supports insert and remove; a list supports append, insert and remove (add when keyed)");
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

static int state_index(const Compiler *c, const char *name) {
    for (unsigned i = 0; i < c->states; ++i) if (!strcmp(c->state_names[i], name)) return (int)i;
    return -1;
}

static int record_index(const Compiler *c, const char *name) {
    for (unsigned i = 0; i < c->record_count; ++i) if (!strcmp(c->records[i].name, name)) return (int)i;
    return -1;
}

static int fn_index(const Compiler *c, int module, const char *name) {
    for (unsigned i = 0; i < c->fn_count; ++i)
        if (c->fns[i].module == module && !strcmp(c->fns[i].name, name)) return (int)i;
    return -1;
}

/* The library a module names by `use name`, or -1. */
static int used_library(const Compiler *c, int module, const char *name) {
    for (unsigned i = 1; i < c->module_count; ++i)
        if (c->modules[module].uses[i] && !strcmp(c->modules[i].name, name)) return (int)i;
    return -1;
}

static const char *declared_kind(const Compiler *c, const char *name) {
    static const char *const states[] = {"state", "text state", "cursor", "list", "list index"};
    if (fn_index(c, c->module, name) >= 0) return "fn";
    if (used_library(c, c->module, name) >= 0) return "library";
    if (c->module) return NULL; /* a library has only fns and uses */
    int state = state_index(c, name);
    if (state >= 0) return states[c->state_types[state]];
    if (record_index(c, name) >= 0) return "record";
    for (unsigned i = 0; i < c->collections; ++i) if (!strcmp(c->collection_names[i], name)) return "collection";
    int input = input_index(c, name);
    if (input >= 0) return c->inputs[input].kind == KIND_KEY ? "key input" :
                           c->inputs[input].kind == KIND_TEXT ? "text input" : "message";
    for (unsigned i = 0; i < c->views; ++i) if (!c->view_table[i] && !strcmp(c->view_names[i], name)) return "view";
    return NULL;
}

static void declare(Compiler *c, const Token *name) {
    if (reserved_name(name->text)) fail_at(c, name->line, name->column, "'%s' is reserved", name->text);
    const char *kind = declared_kind(c, name->text);
    if (kind) fail_at(c, name->line, name->column, "'%s' is already declared as a %s", name->text, kind);
}

static bool key_name(const char *key) {
    static const char *const named[] = {"Space", "Enter", "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown",
                                        "Backspace", "Delete", "Home", "End", "Tab", "Escape", "PageUp", "PageDown"};
    for (size_t i = 0; i < sizeof(named) / sizeof(*named); ++i) if (!strcmp(key, named[i])) return true;
    if (!strncmp(key, "Key", 3)) return key[3] >= 'A' && key[3] <= 'Z' && !key[4];
    if (!strncmp(key, "Digit", 5)) return key[5] >= '0' && key[5] <= '9' && !key[6];
    return false;
}

/* { name: type ... }: u32 and number are fixed, bytes and text of any length
 * (Pure lays them out: docs/design/008-module-format.md). */
static void message_fields(Compiler *c, Input *in) {
    if (!is(c, "{")) return; /* the field block opens on the declaration's line */
    take(c);
    separators(c);
    while (!is(c, "}")) {
        Token name = expect_name(c, "a field name");
        if (reserved_name(name.text)) fail_at(c, name.line, name.column, "'%s' is reserved", name.text);
        for (unsigned i = 0; i < in->fields; ++i)
            if (!strcmp(in->field_names[i], name.text)) fail_at(c, name.line, name.column, "duplicate field '%s'", name.text);
        if (c->field_count == FIELDS) fail_at(c, name.line, name.column, "at most 32 message fields are allowed");
        expect(c, ":", "':'");
        Token type = expect_name(c, "u32, number, bytes or text");
        static const struct { const char *name; int type; } types[] = {
            {"u32", FIELD_U32}, {"number", FIELD_NUMBER}, {"bytes", FIELD_BYTES}, {"text", FIELD_TEXT}};
        int field_type = 0;
        for (size_t t = 0; t < sizeof(types) / sizeof(*types); ++t)
            if (!strcmp(type.text, types[t].name)) field_type = types[t].type;
        if (!field_type) fail_at(c, type.line, type.column, "field types are u32, number, bytes and text");
        memcpy(in->field_names[in->fields], name.text, NAME);
        in->field_types[in->fields++] = field_type;
        ++c->field_count;
        end(c);
    }
    take(c);
}

/* A signed number literal: a numeric state or field default. */
static double signed_number(Compiler *c, const char *what) {
    double sign = 1;
    if (is(c, "-") || is(c, "+")) sign = take(c).text[0] == '-' ? -1 : 1;
    if (c->token.kind != T_NUMBER) fail_at(c, c->token.line, c->token.column, "expected a numeric %s", what);
    return sign * take(c).number;
}

/* A plain string with no {expressions}: a text state or field default. */
static Expr *plain_string(Compiler *c, const char *what) {
    if (c->token.kind != T_STRING) fail_at(c, c->token.line, c->token.column, "%s starts from a string", what);
    Token literal = take(c);
    Expr *value = string(c, &literal);
    if (value->args[0]->kind != E_TEXT || value->args[0]->next)
        fail_at(c, literal.line, literal.column, "%s starts from a plain string without {expressions}", what);
    return value->args[0];
}

/* record Name { field = number; field: text = "..." } */
static void record_declaration(Compiler *c) {
    Token name = expect_name(c, "record name");
    declare(c, &name);
    if (c->record_count == RECORDS) fail_at(c, name.line, name.column, "at most 8 records are allowed");
    Record *r = &c->records[c->record_count];
    memset(r, 0, sizeof(*r));
    memcpy(r->name, name.text, NAME);
    r->line = name.line;
    r->column = name.column;
    expect(c, "{", "'{' on the line of the record name");
    separators(c);
    while (!is(c, "}")) {
        Token field = expect_name(c, "a field name");
        if (reserved_name(field.text)) fail_at(c, field.line, field.column, "'%s' is reserved", field.text);
        if (!strcmp(field.text, "position"))
            fail_at(c, field.line, field.column, "'position' is where an index is; give the field another name");
        for (unsigned i = 0; i < r->fields; ++i)
            if (!strcmp(r->field_names[i], field.text)) fail_at(c, field.line, field.column, "duplicate field '%s'", field.text);
        if (r->fields == FIELDS_PER_RECORD) fail_at(c, field.line, field.column, "a record has at most 16 fields");
        if (c->record_fields == RECORD_FIELDS) fail_at(c, field.line, field.column, "at most 64 record fields are allowed");
        unsigned index = r->fields++;
        memcpy(r->field_names[index], field.text, NAME);
        ++c->record_fields;
        if (is(c, ":")) {
            take(c);
            Token type = expect_name(c, "text");
            if (strcmp(type.text, "text"))
                fail_at(c, type.line, type.column, "a field is a number (field = 0) or text (field: text)");
            r->field_text[index] = true;
            if (is(c, "=")) {
                take(c);
                r->field_string[index] = plain_string(c, "a text field");
            }
        } else {
            expect(c, "=", "'=' and a number, or ': text'");
            r->field_number[index] = signed_number(c, "field default");
        }
        end(c);
    }
    take(c);
    if (!r->fields) fail_at(c, name.line, name.column, "a record needs at least one field");
    ++c->record_count;
}

static void state_declaration(Compiler *c) {
    Token name = expect_name(c, "state name");
    declare(c, &name);
    if (is(c, ":") && lookahead(c)->kind == T_NAME && !strcmp(lookahead(c)->text, "bytes")) {
        take(c);
        take(c);
        expect_word(c, "by");
        expect_word(c, "key");
        if (c->collections == COLLECTIONS) fail_at(c, name.line, name.column, "at most one keyed collection is allowed");
        memcpy(c->collection_names[c->collections++], name.text, NAME);
        return;
    }
    if (c->states == STATES) fail_at(c, name.line, name.column, "at most 32 states are allowed");
    unsigned index = c->states;
    memcpy(c->state_names[index], name.text, NAME);
    c->state_line[index] = name.line;
    c->state_column[index] = name.column;
    if (is(c, ":")) {
        take(c);
        Token type = expect_name(c, "text, cursor, mark, list or index");
        if (!strcmp(type.text, "text")) {
            c->state_types[index] = STATE_TEXT;
            if (is(c, "=")) {
                take(c);
                c->state_text[index] = plain_string(c, "a text state");
            }
        } else if (!strcmp(type.text, "cursor") || !strcmp(type.text, "mark")) {
            /* Both follow edits; text inserted at a cursor goes before it, at a mark after it. */
            c->state_types[index] = STATE_CURSOR;
            c->state_stays[index] = type.text[0] == 'm';
            expect_word(c, "in");
            Token target = expect_name(c, "a text state");
            memcpy(c->state_target[index], target.text, NAME);
        } else if (!strcmp(type.text, "list") || !strcmp(type.text, "index")) {
            c->state_types[index] = type.text[0] == 'l' ? STATE_LIST : STATE_INDEX;
            expect_word(c, "of");
            Token target = expect_name(c, type.text[0] == 'l' ? "a record" : "a list state");
            memcpy(c->state_target[index], target.text, NAME);
            if (type.text[0] == 'l' && word(c, "by")) { /* keyed: items in key order, each key once */
                take(c);
                Token key = expect_name(c, "a field of the record");
                memcpy(c->state_key[index], key.text, NAME);
                c->state_key_line[index] = key.line;
                c->state_key_column[index] = key.column;
            }
        } else fail_at(c, type.line, type.column,
                       "state types are numbers, text, cursor or mark in a text, list of a record, index of a list, "
                       "and bytes by key");
        ++c->states;
        return;
    }
    expect(c, "=", "'='");
    c->state_types[index] = STATE_NUM;
    c->initial[index] = signed_number(c, "state initializer");
    ++c->states;
}

/* view name = each row in list [where condition] { column = value ... }: one
 * row per item, or per item the condition keeps. */
static void table_declaration(Compiler *c, unsigned table) {
    take(c); /* each */
    Token row = expect_name(c, "a name for each item");
    if (reserved_name(row.text)) fail_at(c, row.line, row.column, "'%s' is reserved", row.text);
    expect_word(c, "in");
    Token list = expect_name(c, "a list state");
    memcpy(c->view_row[table], row.text, NAME);
    memcpy(c->view_list[table], list.text, NAME);
    c->view_line[table][0] = row.line;
    c->view_column[table][0] = row.column;
    c->view_line[table][1] = list.line;
    c->view_column[table][1] = list.column;
    if (word(c, "where")) { /* a row only for the items it keeps */
        take(c);
        c->view_filter[table] = expression(c, 0, false);
    }
    expect(c, "{", "'{' on the line of the table");
    separators(c);
    while (!is(c, "}")) {
        Token column = expect_name(c, "a column name");
        if (reserved_name(column.text)) fail_at(c, column.line, column.column, "'%s' is reserved", column.text);
        for (unsigned i = table + 1; i < c->views; ++i)
            if (!strcmp(c->view_names[i], column.text))
                fail_at(c, column.line, column.column, "duplicate column '%s'", column.text);
        if (c->views == VIEWS) fail_at(c, column.line, column.column, "at most 64 views are allowed");
        expect(c, "=", "'='");
        unsigned index = c->views++;
        memcpy(c->view_names[index], column.text, NAME);
        c->view_table[index] = (int)table + 1;
        c->view_value[index] = expression(c, 0, false);
        end(c);
    }
    if (c->views == table + 1) fail_at(c, c->token.line, c->token.column, "a table needs at least one column");
    take(c);
}

/* fn name(params) = value, or fn name = value. */
static void fn_declaration(Compiler *c) {
    Token name = expect_name(c, "fn name");
    declare(c, &name);
    if (!strcmp(name.text, "text") || !strcmp(name.text, "bytes"))
        fail_at(c, name.line, name.column, "'%s' is a built-in conversion", name.text);
    if (c->fn_count == FNS) fail_at(c, name.line, name.column, "at most 64 fns are allowed");
    Fn *f = &c->fns[c->fn_count];
    memset(f, 0, sizeof(*f));
    memcpy(f->name, name.text, NAME);
    f->module = c->module;
    f->line = name.line;
    f->column = name.column;
    if (is(c, "(")) {
        take(c);
        newlines(c);
        while (!is(c, ")")) {
            Token param = expect_name(c, "a parameter name");
            if (reserved_name(param.text)) fail_at(c, param.line, param.column, "'%s' is reserved", param.text);
            for (unsigned i = 0; i < f->param_count; ++i)
                if (!strcmp(f->params[i], param.text))
                    fail_at(c, param.line, param.column, "duplicate parameter '%s'", param.text);
            if (f->param_count == PARAMS) fail_at(c, param.line, param.column, "a fn takes at most 8 parameters");
            memcpy(f->params[f->param_count++], param.text, NAME);
            newlines(c);
            if (!is(c, ",")) break;
            take(c);
            newlines(c);
        }
        expect(c, ")", "')'");
    }
    expect(c, "=", "'='");
    f->body = expression(c, 0, false);
    ++c->fn_count;
}

static void library_declaration(Compiler *c);

/* use name: the fns of name.aup, called as name.fn(...). Each library is read once. */
static void use_declaration(Compiler *c) {
    Token name = expect_name(c, "a library name");
    declare(c, &name);
    int library = -1;
    for (unsigned i = 1; i < c->module_count; ++i) if (!strcmp(c->modules[i].name, name.text)) library = (int)i;
    if (library >= 0 && c->modules[library].parsing)
        fail_at(c, name.line, name.column, "library '%s' uses itself through its uses", name.text);
    if (library < 0) {
        if (c->module_count == MODULES) fail_at(c, name.line, name.column, "at most 8 libraries are allowed");
        const char *source = NULL;
        size_t length = 0;
        char path[AUP_PATH] = "";
        if (!c->load || !c->load(c->load_context, name.text, &source, &length, path))
            fail_at(c, name.line, name.column, "cannot find library '%s' (%s.aup)", name.text, name.text);
        if (length > AUP_SOURCE_LIMIT) fail_at(c, name.line, name.column, "library '%s' exceeds 32768 bytes", name.text);
        library = (int)c->module_count++;
        memcpy(c->modules[library].name, name.text, NAME);
        snprintf(c->modules[library].path, AUP_PATH, "%s", *path ? path : name.text);
        /* Parse it with its own scanner, then resume here. */
        const char *saved_source = c->source;
        size_t saved_length = c->length, saved_at = c->at;
        int saved_line = c->line, saved_column = c->column, saved_module = c->module;
        Token saved_token = c->token, saved_ahead = c->ahead;
        bool saved_has_ahead = c->has_ahead;
        c->source = source;
        c->length = length;
        c->at = 0;
        c->line = c->column = 1;
        c->has_ahead = false;
        c->module = library;
        c->modules[library].parsing = true;
        c->token = scan(c);
        separators(c);
        while (c->token.kind != T_EOF) library_declaration(c);
        c->modules[library].parsing = false;
        c->source = saved_source;
        c->length = saved_length;
        c->at = saved_at;
        c->line = saved_line;
        c->column = saved_column;
        c->module = saved_module;
        c->token = saved_token;
        c->ahead = saved_ahead;
        c->has_ahead = saved_has_ahead;
    }
    c->modules[c->module].uses[library] = true;
}

/* A library declares fns and uses only: it has no state and sees none. */
static void library_declaration(Compiler *c) {
    Token kind = expect_name(c, "fn or use");
    if (!strcmp(kind.text, "fn")) fn_declaration(c);
    else if (!strcmp(kind.text, "use")) use_declaration(c);
    else fail_at(c, kind.line, kind.column, "a library declares only fn and use");
    end(c);
}

static void declaration(Compiler *c) {
    Token kind = expect_name(c, "state, record, input, view, fn, use, or on");
    if (!strcmp(kind.text, "fn")) {
        fn_declaration(c);
    } else if (!strcmp(kind.text, "use")) {
        use_declaration(c);
    } else if (!strcmp(kind.text, "state")) {
        state_declaration(c);
    } else if (!strcmp(kind.text, "record")) {
        record_declaration(c);
    } else if (!strcmp(kind.text, "input")) {
        Token name = expect_name(c, "input name");
        declare(c, &name);
        if (c->input_count == INPUTS) fail_at(c, name.line, name.column, "at most 32 inputs are allowed");
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
        } else if (word(c, "text")) {
            take(c);
            in->kind = KIND_TEXT;
            ++c->input_count;
        } else if (word(c, "every")) {
            /* input tick = every 1 second: ticks while a host runs the app. */
            take(c);
            if (c->token.kind != T_NUMBER) fail_at(c, c->token.line, c->token.column, "expected a period, as in every 1 second");
            Token amount = take(c);
            Token unit = expect_name(c, "ms, second, minute or hour");
            static const struct { const char *name; double scale; } units[] = {
                {"ms", 1}, {"second", 1000}, {"seconds", 1000}, {"minute", 60000}, {"minutes", 60000},
                {"hour", 3600000}, {"hours", 3600000}};
            double scale = 0;
            for (size_t u = 0; u < sizeof(units) / sizeof(*units); ++u)
                if (!strcmp(unit.text, units[u].name)) scale = units[u].scale;
            if (!scale) fail_at(c, unit.line, unit.column, "a timer's unit is ms, second, minute or hour");
            double period = amount.number * scale;
            if (period != floor(period) || period < 1 || period > 86400000)
                fail_at(c, amount.line, amount.column, "a timer ticks every 1 ms to 24 hours, in whole milliseconds");
            in->kind = KIND_TIMER;
            in->period = (uint32_t)period;
            ++c->input_count;
        } else {
            if (!word(c, "key")) fail_at(c, c->token.line, c->token.column, "expected 'key', 'text', 'message' or 'every'");
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
        if (c->views == VIEWS) fail_at(c, name.line, name.column, "at most 64 views are allowed");
        expect(c, "=", "'='");
        unsigned index = c->views++;
        memcpy(c->view_names[index], name.text, NAME);
        if (!word(c, "each")) c->view_value[index] = expression(c, 0, false);
        else table_declaration(c, index);
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
    if (c->code_count == CODE) fail_at(c, line, column, "program exceeds 65535 instructions");
    c->code[c->code_count].op = op;
    c->code[c->code_count].arg = arg;
    c->code[c->code_count++].value = value;
}

static const char *type_name(Type type) {
    static const char *const names[] = {"", "a number", "bytes", "text", "a cursor", "a distance", "an index"};
    return names[type];
}

static uint32_t constant(Compiler *c, const Expr *e) {
    for (unsigned i = 0; i < c->constants; ++i)
        if (c->constant_length[i] == e->size && (!e->size || !memcmp(c->constant_data + c->constant_offset[i], e->bytes, e->size)))
            return i;
    if (c->constants == CONSTANTS) fail_at(c, e->line, e->column, "at most 4096 distinct strings are allowed");
    if (e->size > CONSTANT_BYTES - c->constant_used) fail_at(c, e->line, e->column, "strings exceed 1 MiB");
    if (e->size) memcpy(c->constant_data + c->constant_used, e->bytes, e->size);
    c->constant_offset[c->constants] = (uint32_t)c->constant_used;
    c->constant_length[c->constants] = (uint32_t)e->size;
    c->constant_used += e->size;
    return c->constants++;
}

/* The keyed collection used by name[key] in the current handler. */
static uint32_t collection(Compiler *c, const char *name, int line, int column) {
    int index = -1;
    for (unsigned i = 0; i < c->collections; ++i) if (!strcmp(c->collection_names[i], name)) index = (int)i;
    if (index < 0) {
        const char *kind = declared_kind(c, name);
        if (kind) fail_at(c, line, column, "'%s' is a %s, not a collection", name, kind);
        fail_at(c, line, column, "unknown collection '%s'", name);
    }
    if (c->message < 0 || c->inputs[c->message].kind != KIND_MESSAGE || !c->inputs[c->message].keyed)
        fail_at(c, line, column, "%s[key] needs the handler of a message by key", name);
    return (uint32_t)index;
}

static int text_state(Compiler *c, const char *name, int line, int column) {
    int index = state_index(c, name);
    if (index < 0 || c->state_types[index] != STATE_TEXT) {
        const char *kind = declared_kind(c, name);
        if (kind) fail_at(c, line, column, "'%s' is a %s, not a text state", name, kind);
        fail_at(c, line, column, "unknown text state '%s'", name);
    }
    return index;
}

static int cursor_owner(const Compiler *c, int cursor) { return state_index(c, c->state_target[cursor]); }

/* The list state name[...] indexes; where points at the index for a collection. */
static int list_state(Compiler *c, const char *name, int line, int column, const Expr *where) {
    int index = state_index(c, name);
    if (index >= 0 && c->state_types[index] == STATE_LIST) return index;
    for (unsigned i = 0; i < c->collections; ++i)
        if (!strcmp(c->collection_names[i], name) && where)
            fail_at(c, where->line, where->column, "a collection is indexed by the message key, as in name[key]");
    const char *kind = declared_kind(c, name);
    if (kind) fail_at(c, line, column, "'%s' is a %s, not a list", name, kind);
    fail_at(c, line, column, "unknown list '%s'", name);
}

static const Record *record_of(const Compiler *c, int list) {
    return &c->records[record_index(c, c->state_target[list])];
}

static unsigned record_field(Compiler *c, const Record *r, const char *field, int line, int column) {
    for (unsigned i = 0; i < r->fields; ++i) if (!strcmp(r->field_names[i], field)) return i;
    fail_at(c, line, column, "record '%s' has no field '%s'", r->name, field);
}

static Kind value(Compiler *c, const Expr *e, bool handler, int height);

static void expect_kind(Compiler *c, const Expr *e, Kind actual, Type type) {
    if (actual.type != type) fail_at(c, e->line, e->column, "expected %s, not %s", type_name(type), type_name(actual.type));
}

static Kind number(Compiler *c, const Expr *e, bool handler, int height) {
    Kind kind = value(c, e, handler, height);
    expect_kind(c, e, kind, NUM);
    return kind;
}

/* A cursor of the named text state. */
static void cursor_of(Compiler *c, const Expr *e, bool handler, int height, int text, const char *name) {
    Kind kind = value(c, e, handler, height);
    expect_kind(c, e, kind, CURSOR);
    if (kind.owner != text)
        fail_at(c, e->line, e->column, "a cursor of '%s' cannot point into '%s'", c->state_names[kind.owner], name);
}

/* An index of the named list state. */
static void index_of(Compiler *c, const Expr *e, bool handler, int height, int list) {
    Kind kind = value(c, e, handler, height);
    expect_kind(c, e, kind, INDEX);
    if (kind.owner != list)
        fail_at(c, e->line, e->column, "an index of '%s' cannot point into '%s'", c->state_names[kind.owner],
                c->state_names[list]);
}

/* The item name[e] names: e is an index of the list, or for a keyed list a
 * key, found by LIST_FIND. */
static void item_of(Compiler *c, const Expr *e, bool handler, int height, int list) {
    Kind kind = value(c, e, handler, height);
    int key = c->list_key[list] - 1;
    if (kind.type != INDEX && key >= 0) {
        const Record *r = record_of(c, list);
        Type type = r->field_text[key] ? TEXT : NUM;
        if (kind.type != type)
            fail_at(c, e->line, e->column, "'%s' is keyed by %s, %s: expected %s or an index, not %s", c->state_names[list],
                    r->field_names[key], type_name(type), type_name(type), type_name(kind.type));
        emit(c, e->line, e->column, LIST_FIND, (uint32_t)list, 0);
        return;
    }
    expect_kind(c, e, kind, INDEX);
    if (kind.owner != list)
        fail_at(c, e->line, e->column, "an index of '%s' cannot point into '%s'", c->state_names[kind.owner],
                c->state_names[list]);
}

/* The key field of a keyed list, or NULL. */
static const char *keyed_by(const Compiler *c, int list) {
    return c->list_key[list] ? c->state_key[list] : NULL;
}

/* A fn inlined: its arguments go on the stack once, the body reads them with
 * PICK, and SLIDE leaves only the result. e names the call (E_FN), or is the
 * name of a fn without parameters. */
static Kind inline_fn(Compiler *c, const Expr *e, int module, const char *name, bool handler, int height) {
    int index = fn_index(c, module, name);
    if (index < 0) {
        if (module != c->module) fail_at(c, e->line, e->column, "library '%s' has no fn '%s'", c->modules[module].name, name);
        const char *kind = declared_kind(c, name);
        if (kind) fail_at(c, e->line, e->column, "'%s' is a %s, not a fn", name, kind);
        fail_at(c, e->line, e->column, "unknown fn '%s'", name);
    }
    Fn *f = &c->fns[index];
    unsigned count = e->kind == E_FN ? e->count : 0;
    if (count != f->param_count)
        fail_at(c, e->line, e->column, "%s expects %u argument%s", name, f->param_count, f->param_count == 1 ? "" : "s");
    if (f->active) fail_at(c, e->line, e->column, "fn '%s' calls itself", name);
    Scope scope = {.fn = index, .base = height, .line = e->line, .column = e->column};
    for (unsigned i = 0; i < count; ++i) scope.kinds[i] = value(c, e->args[i], handler, height + (int)i);
    Scope *outer = c->scope;
    int caller = c->module;
    c->scope = &scope;
    c->module = f->module;
    f->active = true;
    Kind result = value(c, f->body, handler, height + (int)count);
    f->active = false;
    c->scope = outer;
    c->module = caller;
    if (count) emit(c, e->line, e->column, SLIDE, count, 0);
    return result;
}

/* A parameter of the fn being inlined, or -1. */
static int parameter(const Compiler *c, const char *name) {
    if (!c->scope) return -1;
    const Fn *f = &c->fns[c->scope->fn];
    for (unsigned i = 0; i < f->param_count; ++i) if (!strcmp(f->params[i], name)) return (int)i;
    return -1;
}

/* A name of the app (a state, input or collection): not visible in a library
 * fn, and hidden by a parameter of the same name. */
static void app_name(Compiler *c, const Expr *e, const char *name) {
    if (c->module) fail_at(c, e->line, e->column, "'%s' is not a parameter; a library fn reads only its parameters", name);
    if (parameter(c, name) >= 0) fail_at(c, e->line, e->column, "'%s' is a parameter here", name);
}

/* Emit an expression; height is the stack height before it. */
static Kind value(Compiler *c, const Expr *e, bool handler, int height) {
    if (height + 1 > STACK) fail_at(c, e->line, e->column, "expression needs more than 64 stack values");
    switch (e->kind) {
    case E_NUMBER: emit(c, e->line, e->column, CONST, 0, e->number); return (Kind){NUM, -1};
    case E_DT: case E_NOW: {
        const char *name = e->kind == E_DT ? "dt" : "now";
        if (c->folding) fail_at(c, e->line, e->column, "%s is not available in count, sum, max or min", name);
        if (!handler)
            fail_at(c, e->line, e->column, e->kind == E_DT ? "dt is only available in handlers" :
                    "now is only available in handlers; a view shows a state that a timer or an event sets");
        emit(c, e->line, e->column, e->kind == E_DT ? DT : NOW, 0, 0);
        return (Kind){NUM, -1};
    }
    case E_TEXT: emit(c, e->line, e->column, TEXT_CONST, constant(c, e), 0); return (Kind){TEXT, -1};
    case E_STRING: {
        int pieces = 0;
        for (const Expr *part = e->args[0]; part; part = part->next) {
            Kind kind = value(c, part, handler, height + (pieces > 0));
            if (kind.type == NUM) emit(c, part->line, part->column, NUM_TEXT, 0, 0);
            else if (kind.type != TEXT)
                fail_at(c, part->line, part->column, "a string shows numbers and text, not %s", type_name(kind.type));
            if (pieces++) emit(c, part->line, part->column, CONCAT, 0, 0);
        }
        return (Kind){TEXT, -1};
    }
    case E_NAME: {
        if (c->folding && c->scope == c->fold_scope && !strcmp(e->name, c->fold_name)) {
            emit(c, e->line, e->column, ROW, (uint32_t)c->fold_list, 0); /* the item a fold's block runs for */
            return (Kind){INDEX, c->fold_list};
        }
        int param = parameter(c, e->name);
        if (param >= 0 && c->folding && c->scope == c->fold_scope)
            fail_at(c, e->line, e->column,
                    "'%s' is a parameter outside this count, sum, max or min; its block reads only its item and states", e->name);
        if (param >= 0) {
            emit(c, e->line, e->column, PICK, (uint32_t)(height - 1 - (c->scope->base + param)), 0);
            return c->scope->kinds[param];
        }
        if (fn_index(c, c->module, e->name) >= 0) return inline_fn(c, e, c->module, e->name, handler, height);
        if (c->module) fail_at(c, e->line, e->column, "'%s' is not a parameter; a library fn reads only its parameters", e->name);
        if (!c->scope && c->row >= 0 && !strcmp(e->name, c->view_row[c->row])) {
            if (c->folding)
                fail_at(c, e->line, e->column, "'%s' is the column's row; a count, sum, max or min reads only its own item", e->name);
            int list = state_index(c, c->view_list[c->row]);
            emit(c, e->line, e->column, ROW, (uint32_t)list, 0);
            return (Kind){INDEX, list};
        }
        int index = state_index(c, e->name);
        if (index >= 0) {
            int type = c->state_types[index];
            if (type == STATE_LIST)
                fail_at(c, e->line, e->column, "'%s' is a list; read %s.count or a field, as in %s[index].field",
                        e->name, e->name, e->name);
            static const uint8_t loads[] = {LOAD, TEXT_LOAD, CUR_LOAD, 0, INDEX_LOAD};
            static const Type types[] = {NUM, TEXT, CURSOR, 0, INDEX};
            emit(c, e->line, e->column, loads[type], (uint32_t)index, 0);
            return (Kind){types[type], type == STATE_CURSOR || type == STATE_INDEX ? cursor_owner(c, index) : -1};
        }
        int input = input_index(c, e->name);
        if (input >= 0 && c->inputs[input].kind == KIND_TEXT) {
            if (input != c->message) fail_at(c, e->line, e->column, "'%s' is only available in its handler", e->name);
            emit(c, e->line, e->column, TEXT_INPUT, 0, 0);
            return (Kind){TEXT, -1};
        }
        const char *kind = declared_kind(c, e->name);
        if (kind) fail_at(c, e->line, e->column, "'%s' is a %s, not a state", e->name, kind);
        fail_at(c, e->line, e->column, "unknown state '%s'", e->name);
    }
    case E_MEMBER: {
        const Expr *base = e->args[0];
        int library = base->kind == E_NAME && parameter(c, base->name) < 0 ? used_library(c, c->module, base->name) : -1;
        if (library >= 0) return inline_fn(c, e, library, e->member, handler, height); /* library.constant */
        if (base->kind == E_NAME && parameter(c, base->name) < 0 && !c->module) {
            int input = input_index(c, base->name);
            if (input >= 0 && c->inputs[input].kind == KIND_MESSAGE) {
                if (input != c->message)
                    fail_at(c, e->line, e->column, "fields of '%s' are only available in its handler", base->name);
                const Input *in = &c->inputs[input];
                for (unsigned i = 0; i < in->fields; ++i)
                    if (!strcmp(in->field_names[i], e->member)) {
                        emit(c, e->line, e->column, FIELD, i, 0);
                        int type = in->field_types[i];
                        return (Kind){type == FIELD_TEXT ? TEXT : type == FIELD_BYTES ? BYTES : NUM, -1};
                    }
                fail_at(c, e->line, e->column, "message '%s' has no field '%s'", base->name, e->member);
            }
            if (input >= 0 && c->inputs[input].kind == KIND_KEY)
                fail_at(c, base->line, base->column, "'%s' is a key input and has no fields", base->name);
            int state = state_index(c, base->name);
            if (state >= 0 && c->state_types[state] == STATE_TEXT &&
                (!strcmp(e->member, "start") || !strcmp(e->member, "end"))) {
                emit(c, e->line, e->column, e->member[0] == 's' ? CUR_START : CUR_END, (uint32_t)state, 0);
                return (Kind){CURSOR, state};
            }
            if (state >= 0 && c->state_types[state] == STATE_LIST) {
                const char *m = e->member;
                if (!strcmp(m, "count")) { emit(c, e->line, e->column, LIST_COUNT, (uint32_t)state, 0); return (Kind){NUM, -1}; }
                if (!strcmp(m, "first") || !strcmp(m, "last")) {
                    emit(c, e->line, e->column, m[0] == 'f' ? INDEX_FIRST : INDEX_LAST, (uint32_t)state, 0);
                    return (Kind){INDEX, state};
                }
                fail_at(c, e->line, e->column, "a list has count, first and last, not '%s'", m);
            }
        }
        Kind inner = value(c, base, handler, height);
        const char *m = e->member;
        if (inner.type == TEXT && (!strcmp(m, "chars") || !strcmp(m, "lines") || !strcmp(m, "bytes"))) {
            emit(c, e->line, e->column, m[0] == 'c' ? CHARS : m[0] == 'l' ? LINES : TEXT_BYTES, 0, 0);
            return (Kind){NUM, -1};
        }
        if (inner.type == BYTES && !strcmp(m, "bytes")) { emit(c, e->line, e->column, LEN, 0, 0); return (Kind){NUM, -1}; }
        if (inner.type == CURSOR && (!strcmp(m, "chars") || !strcmp(m, "line"))) {
            emit(c, e->line, e->column, CUR_COUNT, m[0] == 'c' ? UNIT_CHAR : UNIT_LINE, 0);
            return (Kind){NUM, -1};
        }
        if (inner.type == INDEX && !strcmp(m, "position")) { emit(c, e->line, e->column, INDEX_NUM, 0, 0); return (Kind){NUM, -1}; }
        if (inner.type == INDEX) {
            /* at.title: a field of the item the index names. */
            const Record *r = record_of(c, inner.owner);
            unsigned field = record_field(c, r, m, e->line, e->column);
            emit(c, e->line, e->column, ITEM_GET, (uint32_t)inner.owner | field << 16, 0);
            return (Kind){r->field_text[field] ? TEXT : NUM, -1};
        }
        fail_at(c, e->line, e->column, "%s has no '%s'", type_name(inner.type), m);
    }
    case E_FN: {
        if (!*e->member) return inline_fn(c, e, c->module, e->name, handler, height);
        int library = parameter(c, e->member) < 0 ? used_library(c, c->module, e->member) : -1;
        if (library >= 0) return inline_fn(c, e, library, e->name, handler, height);
        int list = state_index(c, e->member);
        if (!strcmp(e->name, "has") && list >= 0 && c->state_types[list] == STATE_LIST) {
            /* people.has("bob"): whether a keyed list holds that key. */
            app_name(c, e, e->member);
            const char *key = keyed_by(c, list);
            if (!key) fail_at(c, e->line, e->column, "'%s' has no key; declare it as list of %s by a field to ask has",
                              e->member, record_of(c, list)->name);
            if (e->count != 1) fail_at(c, e->line, e->column, "%s.has takes one key", e->member);
            Type type = record_of(c, list)->field_text[c->list_key[list] - 1] ? TEXT : NUM;
            Kind given = value(c, e->args[0], handler, height);
            if (given.type != type)
                fail_at(c, e->args[0]->line, e->args[0]->column, "'%s' is keyed by %s, %s: expected %s, not %s", e->member, key,
                        type_name(type), type_name(type), type_name(given.type));
            emit(c, e->line, e->column, LIST_HAS, (uint32_t)list, 0);
            return (Kind){NUM, -1};
        }
        if (strcmp(e->name, "line")) fail_at(c, e->line, e->column, "unknown library '%s'; write use %s first", e->member, e->member);
        /* note.line(at): the line a cursor is on. */
        app_name(c, e, e->member);
        int text = text_state(c, e->member, e->line, e->column);
        if (e->count != 1) fail_at(c, e->line, e->column, "%s.line takes one cursor", e->member);
        cursor_of(c, e->args[0], handler, height, text, e->member);
        emit(c, e->line, e->column, TEXT_LINE, (uint32_t)text, 0);
        return (Kind){TEXT, -1};
    }
    case E_DOC:
        app_name(c, e, e->name);
        emit(c, e->line, e->column, DOC, collection(c, e->name, e->line, e->column), 0);
        return (Kind){BYTES, -1};
    case E_SIZE:
        app_name(c, e, e->name);
        emit(c, e->line, e->column, DOC_SIZE, collection(c, e->name, e->line, e->column), 0);
        return (Kind){NUM, -1};
    case E_CONVERT: {
        Kind inner = value(c, e->args[0], handler, height);
        Type from = e->op == TEXT_FROM ? BYTES : TEXT;
        if (inner.type != from)
            fail_at(c, e->line, e->column, "%s(...) converts %s, not %s", e->op == TEXT_FROM ? "text" : "bytes",
                    type_name(from), type_name(inner.type));
        emit(c, e->line, e->column, (uint8_t)e->op, 0, 0);
        return (Kind){e->op == TEXT_FROM ? TEXT : BYTES, -1};
    }
    case E_ITEM: {
        app_name(c, e, e->name);
        int list = list_state(c, e->name, e->line, e->column, e->args[0]);
        const Record *r = record_of(c, list);
        if (!*e->member) {
            /* people["bob"]: the index of the item with that key. */
            if (!keyed_by(c, list))
                fail_at(c, e->line, e->column, "read a field of the item, as in %s[index].%s", e->name, r->field_names[0]);
            item_of(c, e->args[0], handler, height, list);
            if (c->code[c->code_count - 1].op != LIST_FIND)
                fail_at(c, e->line, e->column, "%s[index] is the index itself; read a field, as in %s[index].%s", e->name,
                        e->name, r->field_names[0]);
            return (Kind){INDEX, list};
        }
        unsigned field = record_field(c, r, e->member, e->line, e->column);
        item_of(c, e->args[0], handler, height, list);
        emit(c, e->line, e->column, ITEM_GET, (uint32_t)list | field << 16, 0);
        return (Kind){r->field_text[field] ? TEXT : NUM, -1};
    }
    case E_IF: {
        /* Arms of an else-if chain share one end; a value crosses it, so the
         * branches ahead that carry one are counted against Pure's limit. */
        unsigned ends[DEPTH], arms = 0, saved = c->joins;
        Kind kind = {0, -1};
        const Expr *arm = e;
        for (;;) {
            number(c, arm->args[0], handler, height);
            unsigned jump = c->code_count;
            emit(c, arm->line, arm->column, JZ, 0, 0);
            if (height && ++c->joins > JOINS) fail_at(c, arm->line, arm->column, "conditional expressions nest too deeply");
            Kind then = value(c, arm->args[1], handler, height);
            if (arms && (then.type != kind.type || then.owner != kind.owner))
                fail_at(c, arm->args[1]->line, arm->args[1]->column, then.type != kind.type ?
                        "the branches of if give %s and %s" : "the branches of if point into different texts or lists",
                        type_name(kind.type), type_name(then.type));
            kind = then;
            ends[arms++] = c->code_count;
            emit(c, arm->line, arm->column, JMP, 0, 0);
            if (arms == 1 && ++c->joins > JOINS) fail_at(c, arm->line, arm->column, "conditional expressions nest too deeply");
            c->code[jump].arg = c->code_count;
            if (height) --c->joins;
            if (arm->args[2]->kind != E_IF) break;
            arm = arm->args[2];
        }
        Kind otherwise = value(c, arm->args[2], handler, height);
        if (otherwise.type != kind.type || otherwise.owner != kind.owner)
            fail_at(c, arm->args[2]->line, arm->args[2]->column, otherwise.type != kind.type ?
                    "the branches of if give %s and %s" : kind.type == CURSOR ?
                    "the branches of if give cursors of different texts" : "the branches of if give indices of different lists",
                    type_name(kind.type), type_name(otherwise.type));
        for (unsigned i = 0; i < arms; ++i) c->code[ends[i]].arg = c->code_count;
        c->joins = saved;
        return kind;
    }
    case E_FOLD: {
        /* The block after FOLD runs once per item, as a column of the list: it
         * starts with an empty stack and reads only its item and states. */
        app_name(c, e, e->member);
        int list = state_index(c, e->member);
        if (list < 0 || c->state_types[list] != STATE_LIST) fail_at(c, e->line, e->column, "'%s' is not a list", e->member);
        if (c->folding) fail_at(c, e->line, e->column, "a count, sum, max or min cannot hold another");
        if (c->filtering) fail_at(c, e->line, e->column, "a table's where cannot hold a count, sum, max or min");
        unsigned at = c->code_count, joins = c->joins;
        emit(c, e->line, e->column, FOLD, 0, 0);
        c->folding = true;
        c->fold_list = list;
        c->fold_scope = c->scope;
        memcpy(c->fold_name, e->name, NAME);
        c->joins = 0;
        bool itemwise = e->op == FOLD_COUNT || e->op == FOLD_FIRST;
        if (!itemwise) {
            Kind kind = value(c, e->args[1], false, 0);
            if (kind.type != NUM)
                fail_at(c, e->args[1]->line, e->args[1]->column, "sum, max and min take numbers, not %s", type_name(kind.type));
        }
        if (e->args[0]) number(c, e->args[0], false, !itemwise);
        else emit(c, e->line, e->column, CONST, 0, 1);
        c->folding = false;
        c->fold_list = -1;
        c->joins = joins;
        c->code[at].arg = (uint32_t)e->op | (uint32_t)list << 8 | (uint32_t)(c->code_count - at - 1) << 16;
        return e->op == FOLD_FIRST ? (Kind){INDEX, list} : (Kind){NUM, -1};
    }
    case E_RECORD: case E_FIELD:
        fail_at(c, e->line, e->column, "a record value goes into a list, as in list.append(%s { ... })", e->name);
    case E_UNIT:
        fail_at(c, e->line, e->column, "a distance moves a cursor, as in at + 1 char");
    case E_BINARY:
        if (e->op == ADD || e->op == SUB) {
            /* at + 1 char moves a cursor, at + 1 an index; both stop at the ends. */
            bool distance = e->args[1]->kind == E_UNIT;
            Kind left = value(c, e->args[0], handler, height);
            if (distance && left.type != CURSOR)
                fail_at(c, e->line, e->column, "only a cursor moves by a distance, not %s", type_name(left.type));
            if (!distance && left.type != INDEX) expect_kind(c, e->args[0], left, NUM);
            number(c, distance ? e->args[1]->args[0] : e->args[1], handler, height + 1);
            if (left.type == NUM) { emit(c, e->line, e->column, (uint8_t)e->op, 0, 0); return left; }
            if (e->op == SUB) emit(c, e->line, e->column, NEG, 0, 0);
            if (distance) emit(c, e->line, e->column, CUR_MOVE, (uint32_t)e->args[1]->op, 0);
            else emit(c, e->line, e->column, INDEX_MOVE, 0, 0);
            return left;
        }
        if (e->op >= LT && e->op <= NE) {
            /* Texts compare by their bytes; numbers as numbers. */
            Kind left = value(c, e->args[0], handler, height);
            if (left.type == TEXT) {
                expect_kind(c, e->args[1], value(c, e->args[1], handler, height + 1), TEXT);
                emit(c, e->line, e->column, TEXT_COMPARE, (uint32_t)(e->op - LT), 0);
                return (Kind){NUM, -1};
            }
            expect_kind(c, e->args[0], left, NUM);
            number(c, e->args[1], handler, height + 1);
            emit(c, e->line, e->column, (uint8_t)e->op, 0, 0);
            return (Kind){NUM, -1};
        }
        /* fall through */
    default:
        for (unsigned i = 0; i < e->count; ++i) number(c, e->args[i], handler, height + (int)i);
        emit(c, e->line, e->column, (uint8_t)e->op, 0, 0);
        return (Kind){NUM, -1};
    }
}

/* Bytes, or text sent as its UTF-8 bytes. */
static void bytes_value(Compiler *c, const Expr *e, int height, const char *what) {
    Kind kind = value(c, e, true, height);
    if (kind.type == TEXT) emit(c, e->line, e->column, BYTES_FROM, 0, 0);
    else if (kind.type != BYTES) fail_at(c, e->line, e->column, "%s takes bytes or text, not %s", what, type_name(kind.type));
}

/* A record's fields in order, onto the stack from height; missing fields take their default. */
static void record_fields(Compiler *c, const Expr *e, int list, int height) {
    const Record *r = record_of(c, list);
    if (e->kind != E_RECORD)
        fail_at(c, e->line, e->column, "'%s' holds %s records, as in %s.append(%s { %s: ... })", c->state_names[list],
                r->name, c->state_names[list], r->name, r->field_names[0]);
    if (strcmp(e->name, r->name)) fail_at(c, e->line, e->column, "'%s' holds %s, not %s", c->state_names[list], r->name, e->name);
    const Expr *given[FIELDS_PER_RECORD] = {0};
    for (const Expr *f = e->args[0]; f; f = f->next) {
        unsigned field = record_field(c, r, f->member, f->line, f->column);
        if (given[field]) fail_at(c, f->line, f->column, "field '%s' is given twice", f->member);
        given[field] = f;
    }
    for (unsigned k = 0; k < r->fields; ++k) {
        int at = height + (int)k;
        if (given[k]) expect_kind(c, given[k]->args[0], value(c, given[k]->args[0], true, at), r->field_text[k] ? TEXT : NUM);
        else if (at + 1 > STACK) fail_at(c, e->line, e->column, "expression needs more than 64 stack values");
        else if (r->field_text[k]) emit(c, e->line, e->column, TEXT_CONST, r->field_constant[k], 0);
        else emit(c, e->line, e->column, CONST, 0, r->field_number[k]);
    }
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
            if (c->state_types[index] == STATE_NUM) {
                number(c, s->value, true, 0);
                emit(c, s->line, s->column, STORE, (uint32_t)index, 0);
            } else if (c->state_types[index] == STATE_LIST) {
                fail_at(c, s->line, s->column, "a list changes by append, insert, remove and %s[index].field = value", s->name);
            } else if (c->state_types[index] == STATE_INDEX) {
                index_of(c, s->value, true, 0, cursor_owner(c, index));
                emit(c, s->line, s->column, INDEX_STORE, (uint32_t)index, 0);
            } else if (c->state_types[index] == STATE_TEXT) {
                expect_kind(c, s->value, value(c, s->value, true, 0), TEXT);
                emit(c, s->line, s->column, TEXT_SET, (uint32_t)index, 0);
            } else {
                int owner = cursor_owner(c, index);
                cursor_of(c, s->value, true, 0, owner, c->state_names[owner]);
                emit(c, s->line, s->column, CUR_STORE, (uint32_t)index, 0);
            }
            break;
        }
        case S_INSERT: case S_REMOVE: {
            int list = state_index(c, s->name);
            if (list >= 0 && c->state_types[list] == STATE_LIST) {
                if (s->last) fail_at(c, s->last->line, s->last->column, "a list removes one item, as in %s.remove(index)", s->name);
                if (s->kind == S_INSERT && keyed_by(c, list))
                    fail_at(c, s->line, s->column, "'%s' is keyed by %s: %s.add(%s { ... }) places an item by its key", s->name,
                            keyed_by(c, list), s->name, record_of(c, list)->name);
                if (s->kind == S_REMOVE) item_of(c, s->first, true, 0, list);
                else index_of(c, s->first, true, 0, list);
                if (s->kind == S_INSERT) record_fields(c, s->value, list, 1);
                emit(c, s->line, s->column, s->kind == S_INSERT ? LIST_INSERT : LIST_REMOVE, (uint32_t)list, 0);
                break;
            }
            int text = text_state(c, s->name, s->line, s->column);
            cursor_of(c, s->first, true, 0, text, s->name);
            if (s->kind == S_INSERT) {
                expect_kind(c, s->value, value(c, s->value, true, 1), TEXT);
                emit(c, s->line, s->column, TEXT_INSERT, (uint32_t)text, 0);
                break;
            }
            if (!s->last) fail_at(c, s->line, s->column, "a text removes a range, as in %s.remove(first .. last)", s->name);
            cursor_of(c, s->last, true, 1, text, s->name);
            emit(c, s->line, s->column, TEXT_REMOVE, (uint32_t)text, 0);
            break;
        }
        case S_APPEND: case S_ADD: {
            /* A keyed list places an added item by its key (it fails if the key is there); others append. */
            int list = list_state(c, s->name, s->line, s->column, NULL);
            const char *key = keyed_by(c, list), *record = record_of(c, list)->name;
            if (s->kind == S_APPEND && key)
                fail_at(c, s->line, s->column, "'%s' is keyed by %s: %s.add(%s { ... }) places an item by its key", s->name,
                        key, s->name, record);
            if (s->kind == S_ADD && !key)
                fail_at(c, s->line, s->column, "'%s' has no key: append or insert places its items (add needs list of %s by a field)",
                        s->name, record);
            record_fields(c, s->value, list, 0);
            emit(c, s->line, s->column, LIST_APPEND, (uint32_t)list, 0);
            break;
        }
        case S_SET: {
            int list = *s->name ? list_state(c, s->name, s->line, s->column, s->first) : -1;
            if (list < 0) {
                Kind index = value(c, s->first, true, 0);
                expect_kind(c, s->first, index, INDEX);
                list = index.owner;
            }
            const Record *r = record_of(c, list);
            unsigned field = record_field(c, r, s->member, s->line, s->column);
            if (c->list_key[list] == (int)field + 1)
                fail_at(c, s->line, s->column, "the key of '%s' does not change; remove the item and add it again", c->state_names[list]);
            if (*s->name) item_of(c, s->first, true, 0, list);
            expect_kind(c, s->value, value(c, s->value, true, 1), r->field_text[field] ? TEXT : NUM);
            emit(c, s->line, s->column, ITEM_SET, (uint32_t)list | field << 16, 0);
            break;
        }
        case S_REPLACE: {
            uint32_t target = collection(c, s->name, s->line, s->column);
            number(c, s->first, true, 0);
            number(c, s->last, true, 1);
            bytes_value(c, s->value, 2, "replace");
            emit(c, s->line, s->column, REPLACE, target, 0);
            break;
        }
        case S_REPLY:
            if (c->message < 0 || c->inputs[c->message].kind != KIND_MESSAGE)
                fail_at(c, s->line, s->column, "reply is only available in a message handler");
            bytes_value(c, s->value, 0, "reply");
            emit(c, s->line, s->column, REPLY, 0, 0);
            break;
        case S_REQUIRE:
            number(c, s->value, true, 0);
            emit(c, s->line, s->column, REQUIRE, 0, 0);
            break;
        case S_IF: {
            number(c, s->value, true, 0);
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
    uint32_t events[HANDLERS], state_ref[STATES] = {0};
    int handler_input[HANDLERS];
    for (unsigned i = 0; i < c->states; ++i) {
        if (c->state_types[i] == STATE_TEXT) {
            Expr empty = {.kind = E_TEXT, .line = c->state_line[i], .column = c->state_column[i]};
            state_ref[i] = constant(c, c->state_text[i] ? c->state_text[i] : &empty);
        } else if (c->state_types[i] == STATE_CURSOR || c->state_types[i] == STATE_INDEX) {
            bool cursor = c->state_types[i] == STATE_CURSOR;
            int target = state_index(c, c->state_target[i]);
            if (target < 0 || c->state_types[target] != (cursor ? STATE_TEXT : STATE_LIST))
                fail_at(c, c->state_line[i], c->state_column[i], cursor ? "a cursor moves in a text state; '%s' is not one" :
                        "an index points into a list state; '%s' is not one", c->state_target[i]);
            state_ref[i] = (uint32_t)target;
        } else if (c->state_types[i] == STATE_LIST) {
            int record = record_index(c, c->state_target[i]);
            if (record < 0)
                fail_at(c, c->state_line[i], c->state_column[i], "a list holds a record; '%s' is not one", c->state_target[i]);
            state_ref[i] = (uint32_t)record;
            if (*c->state_key[i])
                c->list_key[i] = 1 + (int)record_field(c, &c->records[record], c->state_key[i], c->state_key_line[i],
                                                       c->state_key_column[i]);
        }
    }
    for (unsigned i = 0; i < c->record_count; ++i) {
        Record *r = &c->records[i];
        for (unsigned k = 0; k < r->fields; ++k) {
            Expr empty = {.kind = E_TEXT, .line = r->line, .column = r->column};
            if (r->field_text[k]) r->field_constant[k] = constant(c, r->field_string[k] ? r->field_string[k] : &empty);
        }
    }
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
        const char *what = c->inputs[input].kind == KIND_MESSAGE ? "message" :
                           c->inputs[input].kind == KIND_TIMER ? "timer" : "text input";
        if (c->inputs[input].kind != KIND_KEY && c->handlers[i].edge >= 0)
            fail_at(c, line, column, "'%s' is a %s; write on %s { }", name, what, name);
        if (c->inputs[input].kind == KIND_KEY && c->handlers[i].edge < 0)
            fail_at(c, line, column, "'%s' is a key input; write on %s.down or on %s.up", name, name, name);
        handler_input[i] = input;
        events[i] = 1u + 2u * (uint32_t)input + (c->handlers[i].edge > 0);
    }
    /* Views, then handlers, each as one contiguous block. A table names its
     * list in place of code; its columns may read its row. */
    c->message = -1;
    for (unsigned i = 0; i < c->views; ++i) {
        if (!c->view_value[i]) {
            const char *row = c->view_row[i], *kind = declared_kind(c, row);
            if (kind) fail_at(c, c->view_line[i][0], c->view_column[i][0], "'%s' is already declared as a %s", row, kind);
            int list = list_state(c, c->view_list[i], c->view_line[i][1], c->view_column[i][1], NULL);
            c->view_types[i] = 2;
            c->view_code[i][0] = (uint32_t)list;
            c->view_code[i][1] = 0;
            if (c->view_filter[i]) {
                /* The filter runs as a column of the list: it reads the table's row. */
                c->row = (int)i;
                c->filtering = true;
                c->view_code[i][0] = c->code_count;
                number(c, c->view_filter[i], false, 0);
                c->view_code[i][1] = c->code_count - c->view_code[i][0];
                c->view_byte[i] = (unsigned char)(list + 1);
                c->filtering = false;
            }
            continue;
        }
        c->row = c->view_table[i] - 1;
        c->view_code[i][0] = c->code_count;
        Kind kind = value(c, c->view_value[i], false, 0);
        if (kind.type != NUM && kind.type != TEXT)
            fail_at(c, c->view_value[i]->line, c->view_value[i]->column, "a %s is a number or text, not %s",
                    c->row >= 0 ? "column" : "view", type_name(kind.type));
        c->view_types[i] = kind.type == TEXT;
        c->view_code[i][1] = c->code_count - c->view_code[i][0];
    }
    c->row = -1;
    c->fold_list = -1;
    for (unsigned i = 0; i < c->handler_count; ++i) {
        int input = handler_input[i];
        c->message = input >= 0 && c->inputs[input].kind != KIND_KEY ? input : -1;
        c->handler_code[i][0] = c->code_count;
        statements(c, c->handlers[i].body);
        c->handler_code[i][1] = c->code_count - c->handler_code[i][0];
    }
    static unsigned char strings[(STATES + COLLECTIONS + 2 * INPUTS + FIELDS + VIEWS + RECORDS + RECORD_FIELDS) * NAME];
    size_t used = 0;
    uint32_t state_name[STATES] = {0}, collection_name[COLLECTIONS] = {0}, input_name[INPUTS] = {0}, input_key[INPUTS] = {0};
    uint32_t field_name[FIELDS] = {0}, view_name[VIEWS] = {0}, record_name[RECORDS] = {0};
    uint32_t record_field_name[RECORDS][FIELDS_PER_RECORD] = {{0}};
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
    for (unsigned i = 0; i < c->record_count; ++i) {
        record_name[i] = intern(strings, &used, c->records[i].name);
        for (unsigned k = 0; k < c->records[i].fields; ++k)
            record_field_name[i][k] = intern(strings, &used, c->records[i].field_names[k]);
    }
    *size = 40 + 16u * c->states + 12u * c->collections + 16u * c->input_count + 8u * fields + 8u * c->constants +
            8u * c->record_count + 16u * c->record_fields + 16u * c->views + 12u * c->handler_count + 16u * c->code_count + used + c->constant_used;
    unsigned char *p = *module = allocate(c, *size);
    memcpy(p, "PUREMOD5", 8);
    put16(p + 8, c->states);
    put16(p + 10, c->input_count);
    put16(p + 12, c->views);
    put16(p + 14, c->handler_count);
    put32(p + 16, c->code_count);
    put32(p + 20, (uint32_t)used);
    put16(p + 24, fields);
    put16(p + 26, c->collections);
    put16(p + 28, c->constants);
    put16(p + 30, c->record_count);
    put32(p + 32, (uint32_t)c->constant_used);
    put16(p + 36, c->record_fields);
    p += 40;
    for (unsigned i = 0; i < c->states; ++i) {
        p = put32(p, state_name[i]);
        p = put32(p, (uint32_t)c->state_types[i]);
        if (c->state_types[i] == STATE_NUM) p = put_double(p, c->initial[i]);
        else { /* flags: a mark (1: the cursor stays), or a keyed list's key field + 1 */
            p = put32(p, state_ref[i]);
            p = put32(p, c->state_types[i] == STATE_LIST ? (uint32_t)c->list_key[i] : c->state_stays[i] ? 1 : 0);
        }
    }
    for (unsigned i = 0; i < c->collections; ++i) { p = put32(p, collection_name[i]); p = put32(p, 1); p = put32(p, 1); }
    unsigned first = 0;
    for (unsigned i = 0; i < c->input_count; ++i) {
        const Input *in = &c->inputs[i];
        p = put32(p, input_name[i]);
        *p++ = (unsigned char)in->kind;
        *p++ = in->keyed;
        p = put16(p, 0);
        p = put32(p, in->kind == KIND_TIMER ? in->period : input_key[i]);
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
    for (unsigned i = 0; i < c->constants; ++i) { p = put32(p, c->constant_offset[i]); p = put32(p, c->constant_length[i]); }
    first = 0;
    for (unsigned i = 0; i < c->record_count; ++i) {
        p = put32(p, record_name[i]);
        p = put16(p, first);
        p = put16(p, c->records[i].fields);
        first += c->records[i].fields;
    }
    for (unsigned i = 0; i < c->record_count; ++i)
        for (unsigned k = 0; k < c->records[i].fields; ++k) {
            const Record *r = &c->records[i];
            p = put32(p, record_field_name[i][k]);
            p = put32(p, r->field_text[k]);
            if (r->field_text[k]) { p = put32(p, r->field_constant[k]); p = put32(p, 0); }
            else p = put_double(p, r->field_number[k]);
        }
    for (unsigned i = 0; i < c->views; ++i) {
        p = put32(p, view_name[i]);
        *p++ = (unsigned char)c->view_types[i];
        *p++ = c->view_byte[i];
        p = put16(p, (uint32_t)c->view_table[i]);
        p = put32(p, c->view_code[i][0]);
        p = put32(p, c->view_code[i][1]);
    }
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
    if (c->constant_used) memcpy(p + used, c->constant_data, c->constant_used);
}

bool aup_compile(const char *source, size_t length, AupLoad load, void *context,
                 unsigned char **module, size_t *size, AupDiagnostic *error) {
    *module = NULL;
    *size = 0;
    *error = (AupDiagnostic){.line = 0};
    Compiler *c = calloc(1, sizeof(*c));
    if (!c) { *error = (AupDiagnostic){.line = 1, .column = 1, .message = "out of memory"}; return false; }
    c->source = source;
    c->length = length;
    c->line = c->column = 1;
    c->error = error;
    c->row = -1;
    c->fold_list = -1;
    c->load = load;
    c->load_context = context;
    c->module_count = 1; /* the main source */
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
