#include "aup.h"
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Limits and opcodes of Pure module format v7. Pure counts its states, inputs, views, handlers, records and record fields
 * in u16s alone (027's 4f): the compiler's tables hold these many (their rooms unused but by what a program declares). A
 * message's fields (FIELDS) and a record's (FIELDS_PER_RECORD) are Pure's widths; functions Pure's 64. */
enum { STATES = 4096, INPUTS = 1024, VIEWS = 4096, HANDLERS = 4096, CODE = 65535, STACK = 64, NAME = 32, JOINS = 8,
       DEPTH = 64, FIELDS = 64, CONSTANTS = 4096, CONSTANT_BYTES = 1 << 20,
       RECORDS = 1024, RECORD_FIELDS = 16384, FIELDS_PER_RECORD = 16, FNS = 64, PARAMS = 8, MODULES = 9,
       LIBRARY_RECORDS = 1024, OUTPUTS = 255 };
enum { CONST, LOAD, ADD, SUB, MUL, DIV, NEG, LT, LE, GT, GE, EQ, NE, AND, OR, NOT,
       MIN, MAX, CLAMP, STORE, JZ, JMP, FIELD, LEN, REPLY, REQUIRE,
       TEXT_CONST, TEXT_LOAD, NUM_TEXT, CONCAT, CHARS, LINES, TEXT_BYTES,
       ANCHOR_LOAD, ANCHOR_START, ANCHOR_END, ANCHOR_MOVE, ANCHOR_STORE, TEXT_INSERT, TEXT_REMOVE, TEXT_LINE,
       TEXT_SET, TEXT_FROM, BYTES_FROM, ANCHOR_COUNT, LIST_COUNT, LIST_APPEND, LIST_INSERT, LIST_REMOVE,
       ITEM_GET, ITEM_SET, ROW,
       PICK, SLIDE, FOLD, TEXT_COMPARE, LIST_FIND, LIST_HAS, NOW, FLOOR, MOD, SEND, OUTER_ROW, EACH,
       LOCAL_LOAD, LOCAL_STORE, I64_ADD, I64_SUB, I64_MUL, I64_DIV, I64_MOD, I64_NEG, I64_COMPARE, I64_MIN, I64_MAX,
       I64_FROM, NUM_FROM, LOOP, BREAK, I64_BITS, BYTE_AT, BYTES_OF, ANCHOR_AT, BYTES_SLICE, BYTE, BYTES_LOAD, BYTES_STORE,
       LIST_VALUE, LIST_BIND, APPEND_IN, NUM_PARSE, CALL, RETURN, CHAR_AT, CHAR, FLOORDIV };
/* A cursor's and an index's instructions are Pure's anchor's (027's 4c), MOVE and COUNT naming the text or list in
 * bits 0-15 and the unit above (IN). */
enum { CUR_LOAD = ANCHOR_LOAD, CUR_START = ANCHOR_START, CUR_END = ANCHOR_END, CUR_MOVE = ANCHOR_MOVE, CUR_STORE = ANCHOR_STORE,
       CUR_COUNT = ANCHOR_COUNT, INDEX_LOAD = ANCHOR_LOAD, INDEX_STORE = ANCHOR_STORE, INDEX_MOVE = ANCHOR_MOVE,
       INDEX_FIRST = ANCHOR_START, INDEX_LAST = ANCHOR_END, INDEX_NUM = ANCHOR_COUNT, INDEX_AT = ANCHOR_AT, ANCHOR_UNIT = 16 };
#define IN(target, unit) ((uint32_t)(target) | (uint32_t)(unit) << ANCHOR_UNIT)
/* Pure's values held by name: 0-15 bytes, 16-31 texts; BYTES_STORE's bit 8 appends. NUM_TEXT's format (Pure 027). */
enum { HELD = 16, HELD_TEXT = 16, HELD_APPEND = 1 << 8 };
enum { FORMAT_WIDTH = 8, FORMAT_ZEROS = 1 << 16, FORMAT_FORM = 17, FORMAT_DIGITS = 20,
       FORM_DECIMAL = 1, FORM_HEX, FORM_HEX_UPPER, FORM_BINARY, FORM_OCTAL, FORM_FIXED };
/* Bit operations (Pure's I64_BITS by its argument), apart from the opcodes: ~x is x ^ -1. */
enum { BIT_AND = 0x100, BIT_OR, BIT_XOR, BIT_LEFT, BIT_RIGHT, BIT_NOT };
enum { LOCALS = 64 }; /* an event's locals (Pure's) */
enum { NESTING = 8, LOOP_INTEGER = 256 }; /* loops (for) nest 8 deep; an i64's count (Pure 018) */
enum { FOLD_COUNT, FOLD_SUM, FOLD_MAX, FOLD_MIN, FOLD_FIRST, FOLD_DISTINCT, FOLD_INTEGER = 0x40 };
enum { ABS = BIT_NOT + 1 }; /* abs(x), the compiler's alone: x, a PICK of it, its negation and MAX (no instruction of Pure's) */
/* An input as the app writes it; in Pure's module (027's 4d) each of them but an output and a view's arguments is a
 * message, its binding what makes it: a key's press (KIND_KEY) and its release (KIND_RELEASE, an `on k.up`'s, k.up), a
 * text's (one field, text), a timer's tick, a frame (one field, dt: frame, an `on frame`'s) or a pointing at a table's
 * row (one field, position: T.point, a T_focus view's). */
enum { KIND_KEY = 1, KIND_MESSAGE = 2, KIND_TEXT = 3, KIND_TIMER = 4, KIND_OUTPUT = 5, KIND_ARGUMENTS = 6, KIND_RELEASE = 7,
       KIND_FRAME = 8, KIND_POINT = 9,
       FIELD_U32 = 1, FIELD_BYTES = 2, FIELD_NUMBER = 3, FIELD_TEXT = 4,
       FIELD_ROWS = 5, FIELD_BOOL = 6, FIELD_I64 = 7 };
/* An event's list: a message's rows, filled as the message arrives, or a
 * let's list; emptied when its event ends (Pure's flag). */
enum { LIST_ROWS = 256, LIST_BOUND = 512 };
enum { STATE_TRANSIENT = 256 };
enum { STATE_NUM = 0, STATE_TEXT = 1, STATE_CURSOR = 2, STATE_LIST = 3, STATE_INDEX = 4, STATE_BOOL = 5, STATE_I64 = 6,
       UNIT_CHAR = 0, UNIT_LINE = 1 };
/* Pure's view types, record field types (0 a number, 1 a text, 2 a bool, 3 an i64) and what a local holds. */
/* A view's type is a record field's (Pure 027's 4e). */
enum { VIEW_NUM = 0, VIEW_TEXT = 1, VIEW_BOOL = 2, VIEW_I64 = 3, VIEW_LIST = 4, VIEW_BYTES = 5, LOCAL_BOOL = 2, LOCAL_I64 = 3,
       LOCAL_LIST = 4 };
/* LIST: a list in a record (Pure 027). RECORD: a record's or a variant's value, its fields on the stack in its order
 * (027's 2c); TUPLE: the values a fn gives (owner: its instance); NOTHING: what a fn that gives none gives. */
typedef enum { NUM = 1, BYTES, TEXT, CURSOR, UNIT, INDEX, BOOL, I64, LIST, RECORD, TUPLE, NOTHING } Type;
typedef struct { Type type; int owner; } Kind; /* owner: a cursor's text state, an index's list state, a record's or a list's record */

typedef enum { T_EOF, T_NL, T_NUMBER, T_NAME, T_PUNCT, T_STRING } TokenKind;
typedef struct {
    TokenKind kind;
    int line, column;
    double number;
    int64_t integer; /* a literal of digits alone (whole): its exact value, when not past an i64's range (wide) */
    bool whole, wide, hex; /* hex: written 0x..., an i64 unless a number is wanted where it stands */
    char text[NAME + 1]; /* name, or punctuation of one or two characters */
    size_t start, end;   /* string contents in the source */
} Token;

typedef enum {
    E_NUMBER, E_BOOL, E_NAME, E_DT, E_NOW, E_UNARY, E_BINARY, E_CALL,
    E_MEMBER,  /* args[0].member */
    E_STRING,  /* parts linked by next: E_TEXT literals and expressions */
    E_TEXT,    /* literal bytes */
    E_UNIT,    /* args[0] char|line */
    E_CONVERT, /* text(args[0]) or bytes(args[0]) */
    E_FN,      /* name(args): a fn, qualified by the library in member (name.line(at) when member is a text) */
    E_ITEM,    /* name[args[0]].member: a field of a list item */
    E_RECORD,  /* name { fields }: E_FIELD nodes linked from args[0] */
    E_FIELD,   /* member: args[0] */
    E_IF,      /* if args[0] { args[1] } else { args[2] } */
    E_FOLD,    /* op(args[1] for name in member[args[2] .. args[3]] where args[0]): count, first (no value), sum, max, min,
                  distinct; args[2] and args[3], when given, slice a list keyed by a number */
    E_INDEX,   /* args[0][args[1]]: the byte at a position */
    E_LIST,    /* [ elements linked from args[0] by next ]: a let's list as it begins */
} ExprKind;
typedef struct Expr {
    ExprKind kind;
    int line, column, op;
    double number;
    int64_t integer; /* E_NUMBER: as its token's */
    bool whole, wide, hex;
    bool raw; /* E_MEMBER the compiler made to copy a field: a variant's case read as its number */
    uint32_t format; /* a part of a string: how NUM_TEXT writes it ({x:08x}), past its type */
    char name[NAME], member[NAME];
    struct Expr *args[PARAMS], *next;
    unsigned count;
    unsigned char *bytes;
    size_t size;
} Expr;

typedef enum { S_ASSIGN, S_IF, S_REPLY, S_REQUIRE, S_INSERT, S_REMOVE, S_APPEND, S_ADD, S_SET, S_SEND, S_FOR,
               S_LET, S_COUNT, S_BREAK, S_RETURN, S_ASSIGNS, S_LETS, S_MATCH, S_ARM, S_CALL, S_TOGETHER } StmtKind;
typedef struct Stmt {
    StmtKind kind;
    int line, column;
    char name[NAME], member[NAME]; /* member: the item field an S_SET changes; S_FOR: name is the item, member the list */
    Expr *value;          /* assigned value, condition, reply, requirement or insertion */
    Expr *first, *last;   /* a range, the insertion cursor or index, or the item index; last may be NULL */
    Expr *low, *high;     /* S_FOR: a slice of the list's keys, or NULL; S_COUNT: its range */
                          /* S_SET: name is the list, or empty when first is an index naming the item */
    /* S_LET of a list (listed): member is its record (of library), key the field it is keyed by or empty. */
    bool listed;
    int library, key_line, key_column;
    char key[NAME];
    /* S_ASSIGNS and S_LETS: a, b = x, y gives every name its value at once (the values first, in order): the names, and
     * the values linked from value by next. S_TOGETHER: a, xs[i].x = x, y, its targets (S_ASSIGN, S_SET) from then, each
     * with its value, or value the one call that gives them all. */
    char (*names)[NAME];
    unsigned name_count;
    int refills; /* S_LET of xs = value's copy: xs's list + 1 (its value must be a list of its record), else 0 */
    struct Stmt *then, *otherwise, *next;
} Stmt;

typedef struct {
    char name[NAME], key[NAME];
    int kind;
    uint32_t period; /* a timer's, in milliseconds */
    char field_names[FIELDS][NAME];
    int field_types[FIELDS];
    /* A rows field's record (its library: 0 the app's), where it was written, and the list its rows fill. */
    char field_record[FIELDS][NAME];
    int field_library[FIELDS], field_line[FIELDS], field_column[FIELDS], field_list[FIELDS];
    unsigned fields;
    unsigned view; /* arguments: the view they are for; a pointing's: its table */
    int list, anchor; /* a pointing's: the list its table shows and the index it moves */
} Input;

/* A record: fields in order, each a number, a bool, an i64 or text with its
 * default. Declared in the app (module 0) or in a library; a library's goes
 * into the app's module once a list or a rows field of the app holds it
 * (used: where first). */
typedef struct {
    char name[NAME];
    int line, column, module;
    bool used;
    int used_line, used_column;
    char field_names[FIELDS_PER_RECORD][NAME];
    Type field_kinds[FIELDS_PER_RECORD]; /* NUM, BOOL, I64 or TEXT */
    double field_number[FIELDS_PER_RECORD];
    int64_t field_integer[FIELDS_PER_RECORD];
    Expr *field_string[FIELDS_PER_RECORD]; /* a text default, or NULL for "" */
    uint32_t field_constant[FIELDS_PER_RECORD];
    unsigned fields;
    bool scalar; /* list of i64 (number, bool, text): one field, value, its item read as the value itself */
    /* Pure 027's 2b. A field may hold a record or a variant (its fields then flattened into paths: at.x) or a list of
     * anything (LIST, its items' record in field_record, the module's numbering). As written: declared (a variant's: its
     * cases, each with a value of one type or none), flattened into the fields above once every record is declared. */
    int field_record[FIELDS_PER_RECORD];
    int field_variant[FIELDS_PER_RECORD]; /* a variant's case (its first field): the variant's record + 1, else 0 */
    struct { char name[NAME], type[NAME]; Type kind; bool list, given; double number; int64_t integer; Expr *string; int line, column; }
        declared[FIELDS_PER_RECORD];
    unsigned declared_count;
    bool variant, typed, flattened, flattening;
} Record;

/* A fn: one expression over its parameters, inlined where it is called; or, with a block (statements and returns) or
 * calling itself, a function of Pure's (027's 2c), its results' types written after -> or given by its returns. */
typedef struct {
    char name[NAME], params[PARAMS][NAME];
    unsigned param_count;
    int module, line, column;
    Expr *body;
    Stmt *block;
    struct { char name[NAME]; bool list; int line, column; } results[PARAMS];
    unsigned result_count;
    bool typed;  /* -> its results' types */
    bool active; /* being inlined: a call back into it is recursion */
    bool called; /* run as a function (a block, or a cycle of calls through it) */
} Fn;

typedef struct { uint8_t op; uint32_t arg; union { double value; int64_t integer; }; } Op;

/* A fn run as a function, once for each kinds of arguments it is called with (an item's as its record's value): Pure's
 * function of the same number. Its results are known once declared or a return gave them; its code is its own (ops,
 * count of them: the module's code after the handlers' and views', at start, its jumps moved with it). */
enum { INSTANCES = 64 };
typedef struct {
    int fn;
    Kind params[PARAMS], results[STACK];
    unsigned param_count, result_count;
    bool known;
    Op *ops;
    unsigned count, capacity;
    uint32_t start;
} Instance;

/* The fn being inlined: where its arguments are on the stack and their kinds; while a count, sum, max, min or first
 * in it is compiled, the lets its arguments were put in for it (a fold's block sees no stack: 1 + the let, 0 none). */
typedef struct Scope {
    int fn, base, line, column;
    Kind kinds[PARAMS];
    int spilled[PARAMS];
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
    bool state_transient[STATES]; /* held while the app runs, never stored (Pure 018) */
    bool state_bytes[STATES];     /* a text state of bytes: read and set whole, UTF-8 asked of none (Pure 028) */
    int state_rows[STATES];   /* a list of a message's rows: its input + 1, or 0 */
    bool state_event[STATES]; /* a let's list (let.NAME): its handlers' alone, found only by its let */
    /* Lists the code makes (Pure 027's 2b): a register (bound: Pure's LIST_BIND gives it a list in a record to read) or
     * an empty list (a LIST field's default, LIST_VALUE of nothing), their record already known. */
    bool state_bound[STATES], state_hidden[STATES];
    int state_record[STATES];
    int state_library[STATES]; /* a list's record's library (LIBRARY.NAME), or 0: the app's */
    /* A keyed list: the field named after by, and once resolved, that field + 1. */
    char state_key[STATES][NAME];
    int state_key_line[STATES], state_key_column[STATES], list_key[STATES];
    double initial[STATES];
    int64_t state_integer[STATES]; /* an i64 state's start */
    Expr *state_text[STATES];
    unsigned states;
    Record records[RECORDS]; /* the module's: the app's, then the libraries' it uses (pull_records) */
    unsigned record_count, record_fields;
    Record library_records[LIBRARY_RECORDS]; /* the libraries', in the order they were read */
    unsigned library_record_count;
    Input inputs[INPUTS];
    unsigned input_count, field_count;
    /* Views in order; a table (no value) is followed by its columns, or has none (each ... without them: its list's
     * items as they are, view_whole). */
    char view_names[VIEWS][NAME], view_row[VIEWS][NAME], view_list[VIEWS][NAME];
    Expr *view_value[VIEWS];
    Stmt *view_body[VIEWS];                           /* a view that computes: its statements, return last (view_value) */
    Expr *view_filter[VIEWS];                         /* a table's where condition, or NULL */
    int view_source[VIEWS];                           /* a list view over a list state: that state + 1 */
    int view_record[VIEWS];                           /* a list view's or a list column's items' record + 1 (a map's 0) */
    bool view_whole[VIEWS];                           /* each ... without columns */
    int view_table[VIEWS];                            /* a column's table + 1, or 0 */
    int view_arguments[VIEWS];                        /* the input of a view's arguments + 1, or 0 */
    int view_line[VIEWS][2], view_column[VIEWS][2];   /* a table's row name and list name */
    unsigned views;
    struct { char input[NAME]; int edge, line, column; Stmt *body; } handlers[HANDLERS]; /* body NULL: a pointing's */
    unsigned handler_count;
    /* Code generation. */
    int message;          /* message or text input of the handler being generated, or -1 */
    int arguments;        /* the arguments' input of the view being generated (its table's), or -1 */
    int row;              /* the table whose column is being generated, or -1 */
    /* The fold whose block is being generated: its item's name and list, and
     * the fn scope it began in (whose parameters its block cannot read). */
    bool folding, filtering;
    char fold_name[NAME];
    int fold_list;
    Scope *fold_scope;
    /* A handler's locals in scope (let), innermost last, and the slots it used. A let's list (list: its
     * state) has no slot; a for's count (counter) is read, never assigned. */
    /* (027's 2c) A record's value held by name: the name (its record its kind, no slot) and name.field for each of its
     * fields; variant: a variant's case; alias: a match's name for a case's value (read, never assigned). */
    struct { char name[NAME]; unsigned slot; Kind kind; int list; bool counter, variant, alias; } locals[LOCALS];
    unsigned local_count, local_slots, held_bytes, held_texts; /* held: the bytes' and texts' lets' slots */
    unsigned whiles; /* while loops so far: each counts by a name of its own */
    unsigned settled; /* records' values calls gave, held by names of their own (027's 2c) */
    const struct Expr *given; /* the left side of the && or || whose right side faultless looks at */
    /* A match's bindings (027's 2b): a name for the item's field at path (the item an index in a local, of list), or
     * for its fields under path (prefix: a record's or a variant's). */
    struct { char name[NAME], path[NAME]; int list; unsigned slot; bool prefix; } aliases[16];
    unsigned alias_count;
    /* The fors whose blocks are being generated, innermost last: the item's or count's name, the list a walk
     * walks (-1: a count), and the index local an outer walk's item is kept in for the fors inside it (-1: none). */
    struct { char name[NAME]; int list, item; Scope *scope; } loops[NESTING];
    unsigned loop_count;
    unsigned nested;      /* blocks within the handler's own (an if's, a for's) */
    bool handling;        /* statements of a handler (else of a view's body: they change no state) */
    Kind returned;        /* the value a view's body returned, once it has */
    bool has_returned;
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
    Type want;            /* what the expression value() compiles next is wanted as: an i64 makes a literal one */
    /* The code being made: the handlers' and views' (ops), or a function's (its instance's, growing). */
    Op ops[CODE], *code;
    unsigned code_count, code_capacity;
    Instance instances[INSTANCES];
    unsigned instance_count;
    int current; /* the instance whose code is being made, or -1 */
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
                                       "floor", "output", "send", "for", "let", "true", "false",
                                       "transient", "break", "return", "while", "variant", "match"};

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
    if (ch == '0' && (peek_char(c, 1) == 'x' || peek_char(c, 1) == 'X') && isxdigit((unsigned char)peek_char(c, 2))) {
        /* 0x then hex digits: an i64 (past its range, wide) */
        advance(c);
        advance(c);
        uint64_t exact = 0;
        token.kind = T_NUMBER;
        token.whole = token.hex = true;
        while (isxdigit((unsigned char)peek_char(c, 0))) {
            char h = peek_char(c, 0);
            unsigned v = (unsigned)(digit(h) ? h - '0' : (h | 0x20) - 'a' + 10);
            token.wide = token.wide || exact > (UINT64_C(9223372036854775807) - v) / 16;
            exact = exact * 16 + v;
            advance(c);
        }
        if (letter(peek_char(c, 0))) fail_at(c, token.line, token.column, "a hex number is 0x and the digits 0-9 and a-f");
        token.integer = token.wide ? 0 : (int64_t)exact;
        token.number = (double)exact;
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
        /* Digits alone: exactly, for an i64 (past its range, wide). */
        token.whole = strspn(text, "0123456789") == n;
        uint64_t exact = 0;
        for (size_t i = 0; token.whole && i < n && !token.wide; ++i) {
            unsigned digit = (unsigned)(text[i] - '0');
            token.wide = exact > (UINT64_C(9223372036854775807) - digit) / 10;
            exact = exact * 10 + digit;
        }
        token.integer = token.wide ? 0 : (int64_t)exact;
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
    static const char *const pairs[] = {"<=", ">=", "==", "!=", "&&", "||", "..", "<<", ">>", "->", "//"};
    for (size_t i = 0; i < sizeof(pairs) / sizeof(*pairs); ++i)
        if (ch == pairs[i][0] && peek_char(c, 1) == pairs[i][1]) {
            memcpy(token.text, pairs[i], 3);
            advance(c);
            advance(c);
            token.kind = T_PUNCT;
            return token;
        }
    if (strchr("=+-*/%!<>(){}[],.;:&|^~", ch)) {
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

/* After a declared field (a message's, a group's, a record's): a comma, as
 * between a value's fields, or the end of a line or a semicolon. */
static void end_field(Compiler *c) {
    if (!is(c, ",")) {
        if (c->token.kind != T_NL && !is(c, ";") && !is(c, "}"))
            fail_at(c, c->token.line, c->token.column, "expected a comma, newline or semicolon");
        separators(c);
        return;
    }
    take(c);
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
    {">", 4, GT}, {">=", 4, GE}, {"|", 5, BIT_OR}, {"^", 6, BIT_XOR}, {"&", 7, BIT_AND}, {"<<", 8, BIT_LEFT},
    {">>", 8, BIT_RIGHT}, {"+", 9, ADD}, {"-", 9, SUB}, {"*", 10, MUL}, {"/", 10, DIV}, {"//", 10, FLOORDIV}, {"%", 10, MOD},
};
enum { UNARY = 11 }; /* a unary operator's operand binds tighter than any binary one */

static Expr *node(Compiler *c, ExprKind kind, int line, int column) {
    Expr *e = allocate(c, sizeof(*e));
    e->kind = kind;
    e->line = line;
    e->column = column;
    return e;
}

static Expr *expression(Compiler *c, int minimum, bool multiline);
static Token held_record(Compiler *c, char name[NAME], int *library);
static int used_library(const Compiler *c, int module, const char *name);
static int library_record(const Compiler *c, int library, const char *name);

/* (a, b, ...) after a fn name. */
/* Arguments once the opening parenthesis is taken. */
static Expr *inserted_value(Compiler *c, bool multiline);
static Expr *arguments_after(Compiler *c, Expr *call) {
    newlines(c);
    while (!is(c, ")")) {
        if (call->count == PARAMS) fail_at(c, c->token.line, c->token.column, "a call takes at most 8 arguments");
        call->args[call->count++] = inserted_value(c, true); /* a record's value written out too (027's 2c) */
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
    if (e->kind == E_NAME && is(c, "[")) {
        /* tasks[at].title: a field of a list item. */
        take(c);
        Expr *item = node(c, E_ITEM, e->line, e->column);
        memcpy(item->name, e->name, NAME);
        item->args[0] = expression(c, 0, true);
        item->count = 1;
        newlines(c);
        if (is(c, "..")) { /* data[a .. c]: a slice of a value's bytes */
            take(c);
            item->args[1] = expression(c, 0, true);
            item->count = 2;
            newlines(c);
        }
        expect(c, "]", "']'");
        if (is(c, ".")) {
            take(c);
            memcpy(item->member, expect_name(c, "a field name").text, NAME);
        }
        e = item;
    }
    while (is(c, ".") || is(c, "[")) {
        if (is(c, "[")) { /* packet.data[i]: the byte at a position; data[a .. c] its bytes from a up to before c */
            Token open = take(c);
            Expr *at = node(c, E_INDEX, open.line, open.column);
            at->args[0] = e;
            at->args[1] = expression(c, 0, true);
            at->count = 2;
            newlines(c);
            if (is(c, "..")) {
                take(c);
                at->args[2] = expression(c, 0, true);
                at->count = 3;
                newlines(c);
            }
            expect(c, "]", "']'");
            e = at;
            continue;
        }
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

/* A format after ':' in a string's braces, as Python's: [0][width][.digits][d x X b o f]; how NUM_TEXT writes it.
 * .2d is not Python's: an integer in minor units, its last two digits after a point (money's cents, Pure 027). */
static uint32_t format_of(Compiler *c, const char *text, size_t length, int line, int column) {
    size_t at = 0;
    uint32_t format = 0, width = 0, digits = 0, form = 0;
    if (at < length && text[at] == '0') format |= FORMAT_ZEROS, ++at;
    while (at < length && digit(text[at]) && width <= 255) width = width * 10 + (uint32_t)(text[at++] - '0');
    if (width > 255) fail_at(c, line, column, "a format's width is at most 255");
    if (at < length && text[at] == '.') {
        ++at;
        if (at == length || !digit(text[at])) fail_at(c, line, column, "a format's '.' takes the digits after the point, as in .2f");
        while (at < length && digit(text[at]) && digits <= 30) digits = digits * 10 + (uint32_t)(text[at++] - '0');
        if (digits > 30) fail_at(c, line, column, "a format shows at most 30 digits after the point");
        ++digits; /* Pure's: one more, 0 meaning none */
    }
    if (at < length) {
        const char *forms = "dxXbof", *found = strchr(forms, text[at]);
        if (!found || !text[at]) fail_at(c, line, column, "a format is [0][width][.digits][d x X b o f], as in 08x or .2f");
        form = (uint32_t)(found - forms) + FORM_DECIMAL;
        ++at;
    }
    if (at != length) fail_at(c, line, column, "a format is [0][width][.digits][d x X b o f], as in 08x or .2f");
    if (digits && form != FORM_FIXED && form != FORM_DECIMAL)
        fail_at(c, line, column, "digits after the point go with f or d, as in .2f or .2d (cents: 1234 is 12.34)");
    return format | width << FORMAT_WIDTH | form << FORMAT_FORM | digits << FORMAT_DIGITS | 0x80000000u; /* written */
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
            size_t colon = at + 1; /* {value:format}: the format after the first ':' */
            while (colon < close && c->source[colon] != ':') ++colon;
            c->at = at + 1;
            c->length = colon;
            c->line = token->line;
            c->column = column + 1;
            c->has_ahead = false;
            c->token = scan(c);
            if (c->token.kind == T_EOF) fail_at(c, token->line, column, "empty {expression}");
            Expr *part = expression(c, 0, false);
            if (c->token.kind != T_EOF) fail_at(c, c->token.line, c->token.column, "expected '}'");
            if (colon < close) part->format = format_of(c, c->source + colon + 1, close - colon - 1, token->line,
                                                       token->column + 1 + (int)(colon + 1 - token->start));
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

/* [low .. high] after a list: the items whose keys are from low up to before high. */
static void slice(Compiler *c, Expr **low, Expr **high) {
    take(c); /* [ */
    *low = expression(c, 0, true);
    newlines(c);
    expect(c, "..", "'..'");
    *high = expression(c, 0, true);
    newlines(c);
    expect(c, "]", "']'");
}

/* A list's name, or a message's rows as input.field (a group's too). */
static void list_path(Compiler *c, char out[NAME]) {
    Token list = expect_name(c, "a list");
    size_t length = strlen(list.text);
    memcpy(out, list.text, length + 1);
    while (is(c, ".") && lookahead(c)->kind == T_NAME) {
        take(c);
        Token part = take(c);
        size_t more = strlen(part.text);
        if (length + 1 + more >= NAME)
            fail_at(c, part.line, part.column, "'%s.%s' is longer than %d characters", out, part.text, NAME - 1);
        out[length++] = '.';
        memcpy(out + length, part.text, more + 1);
        length += more;
    }
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
    list_path(c, e->member);
    if (is(c, "[")) slice(c, &e->args[2], &e->args[3]);
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
        left->integer = token.integer;
        left->whole = token.whole;
        left->wide = token.wide;
        left->hex = token.hex;
    } else if (token.kind == T_STRING) {
        left = string(c, &token);
    } else if (token.kind == T_PUNCT && (!strcmp(token.text, "-") || !strcmp(token.text, "!") || !strcmp(token.text, "~"))) {
        left = node(c, E_UNARY, token.line, token.column);
        left->op = token.text[0] == '-' ? NEG : token.text[0] == '!' ? NOT : BIT_NOT;
        left->args[0] = expression(c, UNARY, multiline);
        left->count = 1;
        if (left->op == NEG && left->args[0]->kind == E_NUMBER) { /* -5: one literal, a number or an i64 */
            Expr *literal = left->args[0];
            literal->number = -literal->number;
            literal->integer = -literal->integer;
            literal->line = left->line, literal->column = left->column;
            left = literal;
        }
    } else if (token.kind == T_PUNCT && !strcmp(token.text, "(")) {
        left = expression(c, 0, true);
        newlines(c);
        expect(c, ")", "')'");
    } else if (token.kind == T_PUNCT && !strcmp(token.text, "[")) { /* refused where it stands (only a let's list begins so) */
        left = node(c, E_LIST, token.line, token.column);
        newlines(c);
        for (Expr **tail = &left->args[0]; !is(c, "]"); tail = &(*tail)->next) {
            *tail = expression(c, 0, true);
            newlines(c);
            if (!is(c, ",")) break;
            take(c);
        }
        expect(c, "]", "']'");
    } else if (token.kind == T_NAME && (!strcmp(token.text, "true") || !strcmp(token.text, "false"))) {
        left = node(c, E_BOOL, token.line, token.column);
        left->number = token.text[0] == 't';
    } else if (token.kind == T_NAME) {
        static const struct { const char *name; unsigned count; int op; } calls[] =
            {{"min", 2, MIN}, {"max", 2, MAX}, {"clamp", 3, CLAMP}, {"text", 1, TEXT_FROM}, {"bytes", 1, BYTES_FROM},
             {"floor", 1, FLOOR}, {"i64", 1, I64_FROM}, {"number", 1, NUM_FROM}, {"byte", 1, BYTE}, {"char", 1, CHAR},
             {"abs", 1, ABS}};
        int call = -1, how = -1;
        for (int i = 0; i < (int)(sizeof(calls) / sizeof(*calls)); ++i) if (!strcmp(token.text, calls[i].name)) call = i;
        static const char *const folds[] = {"count", "sum", "max", "min", "first", "distinct"};
        for (int i = 0; i < 6; ++i) if (!strcmp(token.text, folds[i])) how = i;
        Expr *first = NULL;
        /* count(t in tasks ...), first(t in tasks ...), sum(t.x for t in tasks ...),
         * distinct(t.x for t in tasks ...) (how many different values): folds;
         * max(a, b) stays a call. */
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
            if (left->kind == E_CONVERT && is(c, "[")) left = postfix(c, left); /* bytes(t)[i] */
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
/* ..source, last in a record's or a send's braces: an E_FIELD with no
 * member, its value the source (an item, or the handler's message) whose
 * fields of the same names give those not written. */
static bool spread(Compiler *c, Expr ***tail) {
    if (!is(c, "..")) return false;
    Token dots = take(c);
    Expr *f = node(c, E_FIELD, dots.line, dots.column);
    f->args[0] = expression(c, 0, false);
    f->count = 1;
    **tail = f;
    *tail = &f->next;
    while (is(c, ",") || is(c, ";") || c->token.kind == T_NL) take(c);
    if (!is(c, "}")) fail_at(c, c->token.line, c->token.column, "a spread goes last, as in { field: value, ..source }");
    return true;
}

/* NAME { ... }, or LIBRARY.NAME { ... } when library is given (its name in
 * member): a record value, its fields as written. */
static Expr *inserted_value(Compiler *c, bool multiline);

static Expr *record_value(Compiler *c, const Token *library) {
    Token name = take(c);
    take(c); /* { */
    Expr *r = node(c, E_RECORD, name.line, name.column), **tail = &r->args[0];
    memcpy(r->name, name.text, NAME);
    if (library) {
        int used = used_library(c, c->module, library->text);
        if (used < 0)
            fail_at(c, library->line, library->column, "unknown library '%s'; write use %s first", library->text, library->text);
        if (library_record(c, used, name.text) < 0)
            fail_at(c, name.line, name.column, "library '%s' has no record '%s'", library->text, name.text);
        memcpy(r->member, library->text, NAME);
    }
    separators(c);
    while (!is(c, "}")) {
        if (spread(c, &tail)) break;
        Token field = expect_name(c, "a field name");
        expect(c, ":", "':'");
        Expr *f = node(c, E_FIELD, field.line, field.column);
        memcpy(f->member, field.text, NAME);
        f->args[0] = inserted_value(c, false); /* a record's own written out too (027's 2b) */
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

/* send's fields { field: value, group: { ... } }: E_FIELD nodes linked from
 * args[0] of an E_RECORD without a name; a group's value is such a record. */
static Expr *send_fields(Compiler *c) {
    Token open = take(c); /* { */
    Expr *r = node(c, E_RECORD, open.line, open.column), **tail = &r->args[0];
    separators(c);
    while (!is(c, "}")) {
        if (spread(c, &tail)) break;
        Token field = expect_name(c, "a field name");
        expect(c, ":", "':'");
        Expr *f = node(c, E_FIELD, field.line, field.column);
        memcpy(f->member, field.text, NAME);
        f->args[0] = is(c, "{") ? send_fields(c) : expression(c, 0, false);
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

/* A value inserted into a text or a list: an expression or a record
 * (NAME { ... }, or LIBRARY.NAME { ... }: looked at two tokens ahead, the
 * scanner then put back as it was). */
static Expr *inserted_value(Compiler *c, bool multiline);
static Expr *inserted(Compiler *c) { return inserted_value(c, true); }

/* A record value, or an expression that goes into a list (one line unless multiline). */
static Expr *inserted_value(Compiler *c, bool multiline) {
    if (multiline) newlines(c);
    if (c->token.kind == T_NAME && lookahead(c)->kind == T_PUNCT && !strcmp(lookahead(c)->text, "{"))
        return record_value(c, NULL);
    if (c->token.kind == T_NAME && lookahead(c)->kind == T_PUNCT && !strcmp(lookahead(c)->text, ".")) {
        size_t at = c->at;
        int line = c->line, column = c->column;
        Token token = c->token, ahead = c->ahead;
        bool has_ahead = c->has_ahead;
        Token library = take(c);
        take(c); /* . */
        if (c->token.kind == T_NAME && lookahead(c)->kind == T_PUNCT && !strcmp(lookahead(c)->text, "{"))
            return record_value(c, &library);
        c->at = at, c->line = line, c->column = column;
        c->token = token, c->ahead = ahead, c->has_ahead = has_ahead;
    }
    return expression(c, 0, multiline);
}

static Stmt *block(Compiler *c, unsigned depth);

/* name.field.append(item) or name.first.field.append(item) (027's 2b), name taken, '.' next: into the list in a field
 * of the item; false, with nothing taken, for anything else. */
static bool path_append(Compiler *c, Stmt *s, const Token *name) {
    size_t at = c->at;
    int line = c->line, column = c->column;
    Token token = c->token, ahead = c->ahead;
    bool has_ahead = c->has_ahead;
    char parts[8][NAME];
    unsigned count = 0;
    while (is(c, ".") && lookahead(c)->kind == T_NAME && count < 8) {
        take(c);
        Token part = take(c);
        memcpy(parts[count++], part.text, NAME);
    }
    if (count < 2 || strcmp(parts[count - 1], "append") || !is(c, "(")) {
        c->at = at, c->line = line, c->column = column;
        c->token = token, c->ahead = ahead, c->has_ahead = has_ahead;
        return false;
    }
    Expr *root = node(c, E_NAME, name->line, name->column);
    memcpy(root->name, name->text, NAME);
    for (unsigned k = 0; k + 1 < count; ++k) {
        Expr *member = node(c, E_MEMBER, name->line, name->column);
        member->args[0] = root;
        member->count = 1;
        memcpy(member->member, parts[k], NAME);
        root = member;
    }
    take(c); /* ( */
    s->kind = S_APPEND;
    s->first = root;
    s->name[0] = 0;
    s->value = inserted(c);
    newlines(c);
    expect(c, ")", "')'");
    return true;
}

/* , b, c = x, y, z after the first name: every name, then as many values on the line. */
static void several_names(Compiler *c, Stmt *s, const char *first) {
    s->names = allocate(c, STACK * sizeof(*s->names));
    memcpy(s->names[s->name_count++], first, NAME);
    while (is(c, ",")) {
        take(c);
        Token name = expect_name(c, "a name");
        if (reserved_name(name.text)) fail_at(c, name.line, name.column, "'%s' is reserved", name.text);
        if (s->name_count == STACK) fail_at(c, name.line, name.column, "at most %d names take values at once", STACK);
        for (unsigned i = 0; i < s->name_count; ++i)
            if (!strcmp(s->names[i], name.text)) fail_at(c, name.line, name.column, "'%s' is named twice", name.text);
        memcpy(s->names[s->name_count++], name.text, NAME);
    }
}

/* = x, y (after the names): every name's value, or one call that gives them all. */
static void several_values(Compiler *c, Stmt *s) {
    expect(c, "=", "'='");
    Expr **tail = &s->value;
    unsigned values = 0;
    for (;;) {
        *tail = inserted_value(c, false);
        tail = &(*tail)->next;
        ++values;
        if (!is(c, ",")) break;
        take(c);
        newlines(c); /* a line may end after a comma */
    }
    if (values != s->name_count && values != 1) /* one: a fn's call that gives them (027's 2c) */
        fail_at(c, s->line, s->column, "%u names take %u values, not %u", s->name_count, s->name_count, values);
}

/* A target of a, xs[i].x = ...: a name (S_ASSIGN), a list's item's field or its item at a position (S_SET), or an
 * index's field (at.x, S_SET); its value comes later. */
static Stmt *target(Compiler *c, Token name) {
    Stmt *t = allocate(c, sizeof(*t));
    t->line = name.line, t->column = name.column;
    memcpy(t->name, name.text, NAME);
    if (is(c, "[")) {
        take(c);
        t->kind = S_SET;
        t->first = expression(c, 0, true);
        newlines(c);
        expect(c, "]", "']'");
        if (is(c, ".")) {
            take(c);
            Token field = expect_name(c, "a field");
            memcpy(t->member, field.text, NAME);
        }
        return t;
    }
    if (is(c, ".")) {
        take(c);
        Token field = expect_name(c, "a field");
        t->kind = S_SET;
        t->first = node(c, E_NAME, name.line, name.column);
        memcpy(t->first->name, name.text, NAME);
        t->name[0] = 0;
        memcpy(t->member, field.text, NAME);
        return t;
    }
    if (reserved_name(name.text)) fail_at(c, name.line, name.column, "'%s' is reserved", name.text);
    t->kind = S_ASSIGN;
    return t;
}

/* s (its names' count and its values, a, xs[i].x = x, y's) as lets and a statement for each target: the values
 * first, each into a let of its own (a literal, which reads nothing, straight), then each target, left to right. */
static Stmt *through_lets(Compiler *c, Stmt *s, Stmt **targets) {
    unsigned n = s->name_count;
    for (unsigned i = 0; i < n; ++i) snprintf(s->names[i], NAME, "$%d$%d$%u", s->line, s->column, i); /* none of the source's (nor a path) */
    s->kind = S_LETS;
    Expr *values[STACK];
    unsigned count = 0;
    for (Expr *v = s->value; v; v = v->next) values[count++] = v;
    Stmt *last = s;
    Expr **link = &s->value;
    unsigned kept = 0;
    for (unsigned i = 0; i < n; ++i) {
        Expr *v = count == n ? values[i] : NULL; /* NULL: one call gives them all */
        if (v && (v->kind == E_NUMBER || v->kind == E_BOOL || v->kind == E_TEXT)) {
            v->next = NULL;      /* a literal reads nothing: straight to its target */
            targets[i]->value = v;
        } else {
            memcpy(s->names[kept++], s->names[i], NAME);
            Expr *given = node(c, E_NAME, s->line, s->column);
            memcpy(given->name, s->names[i], NAME);
            targets[i]->value = given;
            if (v) *link = v, link = &v->next;
        }
        last = last->next = targets[i];
    }
    last->next = NULL;
    if (count == n) *link = NULL;
    s->name_count = kept;
    if (kept == 1) { /* one let: as let x = value */
        s->kind = S_LET;
        memcpy(s->name, s->names[0], NAME);
        s->names = NULL, s->name_count = 0;
    }
    return kept ? s : s->next; /* every value a literal: no lets */
}

/* a, xs[i].x = x, y after its first target: every target its value at once, the values first, in order. Names alone
 * are S_ASSIGNS; with a field or an item among them, the values go to lets of their own first (a literal, which
 * reads nothing, goes straight), then each target takes its own: a let each and a statement each, linked. */
static Stmt *together(Compiler *c, Stmt *first) {
    Stmt *targets[STACK];
    unsigned n = 0;
    bool names = first->kind == S_ASSIGN;
    targets[n++] = first;
    while (is(c, ",")) {
        take(c);
        Token name = expect_name(c, "a name");
        if (n == STACK) fail_at(c, name.line, name.column, "at most %d names take values at once", STACK);
        Stmt *t = target(c, name);
        for (unsigned i = 0; t->kind == S_ASSIGN && i < n; ++i)
            if (targets[i]->kind == S_ASSIGN && !strcmp(targets[i]->name, t->name))
                fail_at(c, name.line, name.column, "'%s' is named twice", name.text);
        names = names && t->kind == S_ASSIGN;
        targets[n++] = t;
    }
    Stmt *s = allocate(c, sizeof(*s));
    s->line = first->line, s->column = first->column;
    s->names = allocate(c, STACK * sizeof(*s->names));
    s->name_count = n;
    for (unsigned i = 0; names && i < n; ++i) memcpy(s->names[i], targets[i]->name, NAME);
    s->kind = S_ASSIGNS;
    several_values(c, s);
    if (names) return s;
    s->kind = S_TOGETHER; /* compiled straight, or through lets when a value may read what a target changes */
    unsigned count = 0;
    for (const Expr *v = s->value; v; v = v->next) ++count;
    for (unsigned i = 0; count == n && i < n; ++i) {
        Expr *after = s->value->next;
        s->value->next = NULL;
        targets[i]->value = s->value;
        s->value = after;
    }
    for (unsigned i = 0; i < n; ++i) targets[i]->next = i + 1 < n ? targets[i + 1] : NULL;
    s->then = targets[0];
    return s;
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
    if (!strcmp(token.text, "let")) {
        /* let name = value: a number or an index, for the rest of the block */
        s->kind = S_LET;
        Token name = expect_name(c, "a name");
        if (reserved_name(name.text)) fail_at(c, name.line, name.column, "'%s' is reserved", name.text);
        memcpy(s->name, name.text, NAME);
        s->line = name.line;
        s->column = name.column;
        if (is(c, ",")) { /* let a, b = x, y, or let a, b: list of R (a list each, empty as it begins) */
            several_names(c, s, name.text);
            if (!is(c, ":")) {
                s->kind = S_LETS;
                several_values(c, s);
                return s;
            }
        }
        if (is(c, ":")) { /* let name: list of R [by field]: the event's list, empty as it begins */
            take(c);
            Token type = expect_name(c, "list");
            if (strcmp(type.text, "list"))
                fail_at(c, type.line, type.column, "a let holds a list as let %s: list of a record; others take '='", s->name);
            expect_word(c, "of");
            held_record(c, s->member, &s->library);
            if (word(c, "by")) {
                take(c);
                Token key = expect_name(c, "a field of the record");
                memcpy(s->key, key.text, NAME);
                s->key_line = key.line;
                s->key_column = key.column;
            }
            s->listed = true;
            if (s->name_count > 1) { /* the names after the first: lets of their own, of the same list, after it */
                Stmt *last = s;
                for (unsigned i = 1; i < s->name_count; ++i) {
                    Stmt *other = allocate(c, sizeof(*other));
                    *other = *s;
                    other->names = NULL, other->name_count = 0, other->next = NULL; /* (s's next is the first other by now) */
                    memcpy(other->name, s->names[i], NAME);
                    last = last->next = other;
                }
                s->names = NULL, s->name_count = 0;
                if (is(c, "="))
                    fail_at(c, c->token.line, c->token.column, "lists declared together begin empty: give each its elements apart");
                return s;
            }
            if (is(c, "=")) { /* = [a, b, ...]: what it holds as it begins, appended in order */
                take(c);
                Token open = expect(c, "[", "'[' and the list's elements");
                Expr *list = node(c, E_LIST, open.line, open.column), **tail = &list->args[0];
                newlines(c);
                while (!is(c, "]")) {
                    *tail = inserted(c);
                    tail = &(*tail)->next;
                    ++list->count;
                    newlines(c);
                    if (!is(c, ",")) break;
                    take(c);
                    newlines(c);
                }
                expect(c, "]", "']'");
                s->value = list;
            }
            return s;
        }
        expect(c, "=", "'='");
        s->value = inserted_value(c, false);
        return s;
    }
    if (!strcmp(token.text, "match")) {
        /* match item { case(binding) => statement or { block } ... _ => ... }: the arm of the case the item holds */
        s->kind = S_MATCH;
        s->value = expression(c, 0, false);
        expect(c, "{", "'{' and the cases");
        Stmt **tail = &s->then;
        separators(c);
        while (!is(c, "}")) {
            Token label = expect_name(c, "a case, or _ for the rest");
            Stmt *arm = allocate(c, sizeof(*arm));
            arm->kind = S_ARM;
            arm->line = label.line, arm->column = label.column;
            memcpy(arm->name, label.text, NAME);
            if (is(c, "(")) {
                take(c);
                Token binding = expect_name(c, "a name for the case's value");
                if (reserved_name(binding.text)) fail_at(c, binding.line, binding.column, "'%s' is reserved", binding.text);
                memcpy(arm->member, binding.text, NAME);
                arm->key_line = binding.line, arm->key_column = binding.column;
                expect(c, ")", "')'");
            }
            if (!is(c, "=")) fail_at(c, c->token.line, c->token.column, "expected '=>' after the case");
            take(c);
            expect(c, ">", "'>' of '=>'");
            if (is(c, "{")) arm->then = block(c, depth + 1);
            else arm->then = statement(c, depth + 1);
            *tail = arm;
            tail = &arm->next;
            separators(c);
        }
        take(c);
        return s;
    }
    if (!strcmp(token.text, "while")) {
        /* while condition { ... }: a count from 0 up to 2^53 whose block begins if !condition { break }; the budget ends
         * one that never stops */
        s->kind = S_COUNT;
        snprintf(s->name, NAME, "while.%u", c->whiles++);
        s->low = node(c, E_NUMBER, token.line, token.column);
        s->low->whole = true;
        s->high = node(c, E_NUMBER, token.line, token.column);
        s->high->number = 9007199254740992.0;
        s->high->integer = INT64_C(9007199254740992);
        s->high->whole = true;
        Stmt *check = allocate(c, sizeof(*check)), *stop = allocate(c, sizeof(*stop));
        check->kind = S_IF;
        check->line = token.line, check->column = token.column;
        Expr *not = node(c, E_UNARY, token.line, token.column);
        not->op = NOT;
        memcpy(not->member, "while", 6); /* what takes the bool, for its message */
        not->args[0] = expression(c, 0, false);
        not->count = 1;
        check->value = not;
        stop->kind = S_BREAK;
        stop->line = token.line, stop->column = token.column;
        check->then = stop;
        check->next = block(c, depth + 1);
        s->then = check;
        return s;
    }
    if (!strcmp(token.text, "break")) { /* ends the innermost for */
        s->kind = S_BREAK;
        return s;
    }
    if (!strcmp(token.text, "return")) { /* a view's body's last line: its value; in a fn's block, what it gives (none or several) */
        s->kind = S_RETURN;
        if (c->token.kind == T_NL || is(c, "}") || is(c, ";")) return s;
        Expr **tail = &s->value;
        for (;;) {
            *tail = inserted_value(c, false);
            tail = &(*tail)->next;
            if (!is(c, ",")) break;
            take(c);
            newlines(c);
        }
        return s;
    }
    if (!strcmp(token.text, "for")) {
        /* for item in list [where condition] { ... }: the block runs once for every item the list has */
        s->kind = S_FOR;
        Token item = expect_name(c, "a name for each item");
        if (reserved_name(item.text)) fail_at(c, item.line, item.column, "'%s' is reserved", item.text);
        memcpy(s->name, item.text, NAME);
        s->last = node(c, E_NAME, item.line, item.column); /* where the item is named */
        if (!word(c, "in")) fail_at(c, c->token.line, c->token.column, "expected 'in' and a list, as in for %s in tasks { }",
                                    item.text);
        take(c);
        /* for i in low .. high counts; for x in list walks (a list's name, then '{', '[' or where: looked at, the
         * scanner then put back when it counts) */
        bool walks = false;
        if (c->token.kind == T_NAME) {
            size_t at = c->at;
            int line = c->line, column = c->column;
            Token token = c->token, ahead = c->ahead;
            bool has_ahead = c->has_ahead;
            char path[NAME];
            list_path(c, path);
            walks = is(c, "{") || is(c, "[") || word(c, "where");
            c->at = at, c->line = line, c->column = column;
            c->token = token, c->ahead = ahead, c->has_ahead = has_ahead;
        }
        if (!walks) {
            s->kind = S_COUNT;
            s->low = expression(c, 0, false);
            if (!is(c, "..")) fail_at(c, c->token.line, c->token.column, "expected '..', as in for %s in 0 .. n { }", item.text);
            take(c);
            s->high = expression(c, 0, false);
            s->then = block(c, depth + 1);
            return s;
        }
        s->first = node(c, E_NAME, c->token.line, c->token.column); /* where the list is named */
        list_path(c, s->member);
        if (is(c, "[")) slice(c, &s->low, &s->high);
        if (word(c, "where")) {
            take(c);
            s->value = expression(c, 0, false);
        }
        s->then = block(c, depth + 1);
        return s;
    }
    if (!strcmp(token.text, "send")) {
        /* send output { field: value, ... }: every field of the output, groups as their own braces. */
        s->kind = S_SEND;
        Token name = expect_name(c, "an output");
        memcpy(s->name, name.text, NAME);
        if (!is(c, "{")) fail_at(c, c->token.line, c->token.column, "expected '{' and the fields, as in send %s { field: value }", name.text);
        s->value = send_fields(c);
        return s;
    }
    if (!strcmp(token.text, "reply") || !strcmp(token.text, "require")) {
        s->kind = token.text[2] == 'p' ? S_REPLY : S_REQUIRE;
        s->value = expression(c, 0, false);
        return s;
    }
    if (reserved_name(token.text)) fail_at(c, token.line, token.column, "expected a statement or '}'");
    memcpy(s->name, token.text, NAME);
    if (is(c, "(")) { /* f(x): a fn's call for what it does (027's 2c) */
        s->kind = S_CALL;
        Expr *call = node(c, E_FN, token.line, token.column);
        memcpy(call->name, token.text, NAME);
        s->value = arguments(c, call);
        return s;
    }
    if (is(c, "[")) {
        /* tasks[at].field = value */
        take(c);
        s->kind = S_SET;
        s->first = expression(c, 0, true);
        newlines(c);
        expect(c, "]", "']'");
        if (is(c, ".") && lookahead(c)->kind == T_NAME) {
            take(c);
            Token field = take(c);
            if (is(c, ".")) { /* xs[i].items.append(item): into the list in a field of the item (027's 2b) */
                Expr *root = node(c, E_ITEM, s->line, s->column);
                memcpy(root->name, s->name, NAME);
                memcpy(root->member, field.text, NAME);
                root->args[0] = s->first;
                root->count = 1;
                take(c);
                Token method = expect_name(c, "append");
                if (strcmp(method.text, "append")) fail_at(c, method.line, method.column, "a list in a field takes append, for now");
                expect(c, "(", "'('");
                s->kind = S_APPEND;
                s->first = root;
                s->name[0] = 0;
                s->value = inserted(c);
                newlines(c);
                expect(c, ")", "')'");
                return s;
            }
            if (is(c, "(")) fail_at(c, field.line, field.column, "a field changes by '=', as in %s[at].%s = value", s->name, field.text);
            memcpy(s->member, field.text, NAME);
            if (is(c, ",")) return together(c, s); /* xs[i].x, xs[i].y = a, b */
        } else if (is(c, ",")) {
            return together(c, s); /* xs[i], ys[j] = a, b */
        } else if (is(c, "=")) { /* tasks[at] = Task { ... }: every field of the item (no member); xs[i] = 5 */
            take(c);
            s->value = inserted_value(c, false);
            return s;
        } else
            fail_at(c, c->token.line, c->token.column,
                    "expected '.' and the field to change, as in %s[at].field = value, or '=' and a record", s->name);
        expect(c, "=", "'='");
        s->value = expression(c, 0, false);
        return s;
    }
    if (is(c, ".") && path_append(c, s, &token)) return s;
    if (is(c, ".")) {
        take(c);
        Token method = expect_name(c, "insert, remove, append or a field");
        if (is(c, ",")) { /* at.x, at.y = a, b */
            s->kind = S_SET;
            s->first = node(c, E_NAME, token.line, token.column);
            memcpy(s->first->name, s->name, NAME);
            s->name[0] = 0;
            memcpy(s->member, method.text, NAME);
            return together(c, s);
        }
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
    if (is(c, ",")) { /* a, b = b, a; a, xs[i].x = 1, 2 */
        s->kind = S_ASSIGN;
        return together(c, s);
    }
    s->kind = S_ASSIGN;
    expect(c, "=", "'='");
    s->value = inserted_value(c, false);
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
        while (*tail) tail = &(*tail)->next; /* let a, b: list of R is a let each */
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
    for (unsigned i = c->local_count; i-- > 0;) /* a let's list, by the let's name */
        if (c->locals[i].list >= 0 && !strcmp(c->locals[i].name, name)) return c->locals[i].list;
    for (unsigned i = 0; i < c->states; ++i) if (!c->state_event[i] && !strcmp(c->state_names[i], name)) return (int)i;
    return -1;
}

/* A library's record, by name: its place among the libraries' records, or -1. */
static int library_record(const Compiler *c, int library, const char *name) {
    for (unsigned i = 0; i < c->library_record_count; ++i)
        if (c->library_records[i].module == library && !strcmp(c->library_records[i].name, name)) return (int)i;
    return -1;
}

/* A record as the file of module `from` writes it: its name, or LIBRARY.NAME for a library's. */
static const char *record_shown(const Compiler *c, int from, const Record *r) {
    static char shown[2 * NAME + 1];
    if (r->module == from) return r->name;
    snprintf(shown, sizeof(shown), "%s.%s", c->modules[r->module].name, r->name);
    return shown;
}

/* The module's record of a library (0: the app) by name, or -1. */
static int module_record(const Compiler *c, int library, const char *name) {
    for (unsigned i = 0; i < c->record_count; ++i)
        if (c->records[i].module == library && !strcmp(c->records[i].name, name)) return (int)i;
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
    static const char *const states[] = {"a state", "a text state", "a cursor", "a list", "a list index", "a state", "a state"};
    if (fn_index(c, c->module, name) >= 0) return "a fn";
    if (used_library(c, c->module, name) >= 0) return "a library";
    if (c->module) return library_record(c, c->module, name) >= 0 ? "a record" : NULL; /* fns, records and uses */
    int state = state_index(c, name);
    if (state >= 0) return c->state_types[state] == STATE_TEXT && c->state_bytes[state] ? "a bytes state" : states[c->state_types[state]];
    if (record_index(c, name) >= 0) return "a record";
    int input = input_index(c, name);
    if (input >= 0) return c->inputs[input].kind == KIND_KEY ? "a key input" :
                           c->inputs[input].kind == KIND_TEXT ? "a text input" :
                           c->inputs[input].kind == KIND_OUTPUT ? "an output" : "a message";
    for (unsigned i = 0; i < c->views; ++i) if (!c->view_table[i] && !strcmp(c->view_names[i], name)) return "a view";
    return NULL;
}

static void declare(Compiler *c, const Token *name) {
    if (reserved_name(name->text)) fail_at(c, name->line, name->column, "'%s' is reserved", name->text);
    const char *kind = declared_kind(c, name->text);
    if (kind) fail_at(c, name->line, name->column, "'%s' is already declared as %s", name->text, kind);
}

static bool key_name(const char *key) {
    static const char *const named[] = {"Space", "Enter", "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown",
                                        "Backspace", "Delete", "Home", "End", "Tab", "Escape", "PageUp", "PageDown"};
    for (size_t i = 0; i < sizeof(named) / sizeof(*named); ++i) if (!strcmp(key, named[i])) return true;
    if (!strncmp(key, "Key", 3)) return key[3] >= 'A' && key[3] <= 'Z' && !key[4];
    if (!strncmp(key, "Digit", 5)) return key[5] >= '0' && key[5] <= '9' && !key[6];
    return false;
}

/* Whether a message already has a field or a group of this path. */
static bool path_taken(const Input *in, const char *path) {
    size_t length = strlen(path);
    for (unsigned i = 0; i < in->fields; ++i) {
        const char *name = in->field_names[i];
        size_t size = strlen(name);
        if (!strcmp(name, path) || (size > length && !memcmp(name, path, length) && name[length] == '.') ||
            (length > size && !memcmp(path, name, size) && path[size] == '.')) return true;
    }
    return false;
}

/* { name: type ... }: u32 and number are fixed, bytes and text of any length
 * (Pure lays them out: docs/design/008-module-format.md). name: { ... } is a
 * group of fields inside the message, named group.name (a JSON object inside
 * a request's body). */
/* message like other: the fields of a message input or output declared
 * before, in its order. So an output and the input that takes what another
 * node sends from it are written once. */
static void message_like(Compiler *c, Input *in) {
    Token other = expect_name(c, "a message input or output declared before");
    const Input *from = NULL;
    for (unsigned i = 0; i + 1 < c->input_count && !from; ++i)
        if (!strcmp(c->inputs[i].name, other.text)) from = &c->inputs[i];
    if (!from || (from->kind != KIND_MESSAGE && from->kind != KIND_OUTPUT))
        fail_at(c, other.line, other.column, "'%s' is no message input or output declared before", other.text);
    for (unsigned f = 0; f < from->fields; ++f) {
        if (in->kind == KIND_OUTPUT && from->field_types[f] == FIELD_ROWS)
            fail_at(c, other.line, other.column, "an output sends no rows: '%s' has rows (%s)", other.text,
                    from->field_names[f]);
        if (c->field_count == FIELDS) fail_at(c, other.line, other.column, "at most %d message fields are allowed", FIELDS);
        memcpy(in->field_names[in->fields], from->field_names[f], NAME);
        memcpy(in->field_record[in->fields], from->field_record[f], NAME);
        in->field_library[in->fields] = from->field_library[f];
        in->field_line[in->fields] = from->field_line[f];
        in->field_column[in->fields] = from->field_column[f];
        in->field_types[in->fields++] = from->field_types[f];
        ++c->field_count;
    }
}

static void message_fields(Compiler *c, Input *in, const char *group) {
    if (!is(c, "{")) return; /* the field block opens on the declaration's line */
    take(c);
    separators(c);
    unsigned first = in->fields;
    while (!is(c, "}")) {
        Token name = expect_name(c, "a field name");
        if (reserved_name(name.text)) fail_at(c, name.line, name.column, "'%s' is reserved", name.text);
        char path[NAME];
        int length = snprintf(path, sizeof(path), "%s%s%s", group, *group ? "." : "", name.text);
        if (length < 0 || length >= NAME)
            fail_at(c, name.line, name.column, "'%s.%s' is longer than %d characters", group, name.text, NAME - 1);
        if (path_taken(in, path)) fail_at(c, name.line, name.column, "duplicate field '%s'", path);
        expect(c, ":", "':'");
        if (is(c, "{")) {
            message_fields(c, in, path);
            end_field(c);
            continue;
        }
        if (c->field_count == FIELDS) fail_at(c, name.line, name.column, "at most %d message fields are allowed", FIELDS);
        Token type = expect_name(c, "u32, i64, number, bool, bytes, text or list of a record");
        static const struct { const char *name; int type; } types[] = {
            {"u32", FIELD_U32}, {"i64", FIELD_I64}, {"number", FIELD_NUMBER}, {"bool", FIELD_BOOL}, {"bytes", FIELD_BYTES},
            {"text", FIELD_TEXT}};
        int field_type = 0;
        for (size_t t = 0; t < sizeof(types) / sizeof(*types); ++t)
            if (!strcmp(type.text, types[t].name)) field_type = types[t].type;
        if (!strcmp(type.text, "list")) {
            /* rows: list of a record, a JSON array of objects; the handler walks them as a list */
            if (in->kind == KIND_OUTPUT) fail_at(c, type.line, type.column, "an output sends no rows: send one message a row");
            expect_word(c, "of");
            Token record = held_record(c, in->field_record[in->fields], &in->field_library[in->fields]);
            in->field_line[in->fields] = record.line;
            in->field_column[in->fields] = record.column;
            field_type = FIELD_ROWS;
        }
        if (!field_type) fail_at(c, type.line, type.column, "field types are u32, i64, number, bool, bytes, text and list of a record");
        memcpy(in->field_names[in->fields], path, NAME);
        in->field_types[in->fields++] = field_type;
        ++c->field_count;
        end_field(c);
    }
    if (*group && in->fields == first) fail_at(c, c->token.line, c->token.column, "the group '%s' has no fields", group);
    take(c);
}


/* A signed number literal: a numeric state or field default. */
static double signed_number(Compiler *c, const char *what) {
    double sign = 1;
    if (is(c, "-") || is(c, "+")) sign = take(c).text[0] == '-' ? -1 : 1;
    if (c->token.kind != T_NUMBER) fail_at(c, c->token.line, c->token.column, "expected a numeric %s", what);
    return sign * take(c).number;
}

/* A signed integer literal in an i64's range: an i64 state's or field's start. */
static int64_t signed_integer(Compiler *c, const char *what) {
    bool negative = false;
    if (is(c, "-") || is(c, "+")) negative = take(c).text[0] == '-';
    if (c->token.kind != T_NUMBER || !c->token.whole)
        fail_at(c, c->token.line, c->token.column, "expected a whole number for %s", what);
    if (c->token.wide) fail_at(c, c->token.line, c->token.column, "the number is past an i64's range");
    int64_t value = take(c).integer;
    return negative ? -value : value;
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

/* record Name { field = number, field = false, field: text = "..." } */
/* A field's type after ':' (a record's field or a variant's case value): text, i64, number, bool, list of X or X. */
static void declared_type(Compiler *c, Record *r, unsigned index, bool variant) {
    Token type = expect_name(c, variant ? "the type of the case's value" : "text, bytes, i64, a record, a variant or list of one");
    if (!strcmp(type.text, "list")) {
        expect_word(c, "of");
        Token of = expect_name(c, "what the list holds");
        memcpy(r->declared[index].type, of.text, NAME);
        r->declared[index].list = r->typed = true;
        r->declared[index].kind = LIST;
        return;
    }
    if (!strcmp(type.text, "text")) r->declared[index].kind = TEXT;
    else if (!strcmp(type.text, "bytes")) r->declared[index].kind = BYTES; /* (Pure 027's 4b) */
    else if (!strcmp(type.text, "i64")) r->declared[index].kind = I64;
    else if (variant && !strcmp(type.text, "number")) r->declared[index].kind = NUM;
    else if (variant && !strcmp(type.text, "bool")) r->declared[index].kind = BOOL;
    else { /* a record's or a variant's fields, flattened once all are declared */
        if (reserved_name(type.text) || !strcmp(type.text, "number") || !strcmp(type.text, "bool"))
            fail_at(c, type.line, type.column, "a field is a number (field = 0), a bool (field = false), text (field: text), "
                    "bytes (field: bytes), an i64 (field: i64), a record, a variant or a list of one");
        memcpy(r->declared[index].type, type.text, NAME);
        r->typed = true;
    }
}

static void record_declaration(Compiler *c) {
    Token name = expect_name(c, "record name");
    declare(c, &name);
    bool library = c->module != 0; /* a library's: into the module once the app holds it in a list */
    if (!library && c->record_count == RECORDS) fail_at(c, name.line, name.column, "at most %d records are allowed", RECORDS);
    if (library && c->library_record_count == LIBRARY_RECORDS)
        fail_at(c, name.line, name.column, "the libraries declare at most %d records", LIBRARY_RECORDS);
    Record *r = library ? &c->library_records[c->library_record_count] : &c->records[c->record_count];
    memset(r, 0, sizeof(*r));
    memcpy(r->name, name.text, NAME);
    r->line = name.line;
    r->column = name.column;
    r->module = c->module;
    expect(c, "{", "'{' on the line of the record name");
    separators(c);
    while (!is(c, "}")) {
        Token field = expect_name(c, "a field name");
        if (reserved_name(field.text)) fail_at(c, field.line, field.column, "'%s' is reserved", field.text);
        if (!strcmp(field.text, "position"))
            fail_at(c, field.line, field.column, "'position' is where an index is; give the field another name");
        for (unsigned i = 0; i < r->declared_count; ++i)
            if (!strcmp(r->declared[i].name, field.text)) fail_at(c, field.line, field.column, "duplicate field '%s'", field.text);
        if (r->declared_count == FIELDS_PER_RECORD) fail_at(c, field.line, field.column, "a record has at most 16 fields");
        unsigned index = r->declared_count++;
        memcpy(r->declared[index].name, field.text, NAME);
        r->declared[index].line = field.line, r->declared[index].column = field.column;
        if (is(c, ":")) {
            take(c);
            declared_type(c, r, index, false);
            if (is(c, "=")) {
                if (r->declared[index].kind != TEXT && r->declared[index].kind != BYTES && r->declared[index].kind != I64)
                    fail_at(c, c->token.line, c->token.column, "a record, a variant or a list in a field begins as its own default");
                take(c);
                r->declared[index].given = true;
                if (r->declared[index].kind == TEXT || r->declared[index].kind == BYTES)
                    r->declared[index].string = plain_string(c, r->declared[index].kind == TEXT ? "a text field" : "a bytes field");
                else r->declared[index].integer = signed_integer(c, "an i64 field's default");
            }
        } else {
            expect(c, "=", "'=' and a number or a bool, or ': text', ': i64', ': a record' or ': list of a record'");
            r->declared[index].kind = NUM;
            r->declared[index].given = true;
            if (word(c, "true") || word(c, "false")) {
                r->declared[index].kind = BOOL;
                r->declared[index].number = take(c).text[0] == 't';
            } else r->declared[index].number = signed_number(c, "field default");
        }
        end_field(c);
    }
    take(c);
    if (!r->declared_count) fail_at(c, name.line, name.column, "a record needs at least one field");
    if (library && r->typed) fail_at(c, name.line, name.column, "a library's record holds numbers, bools, i64s and texts alone, for now");
    if (!r->typed) { /* plain: its fields as written */
        for (unsigned i = 0; i < r->declared_count; ++i) {
            if (!library && c->record_fields == RECORD_FIELDS)
                fail_at(c, r->declared[i].line, r->declared[i].column, "at most %d record fields are allowed", RECORD_FIELDS);
            memcpy(r->field_names[i], r->declared[i].name, NAME);
            r->field_kinds[i] = r->declared[i].kind;
            r->field_number[i] = r->declared[i].number;
            r->field_integer[i] = r->declared[i].integer;
            r->field_string[i] = r->declared[i].string;
            c->record_fields += !library;
        }
        r->fields = r->declared_count;
        r->flattened = true;
    }
    if (library) ++c->library_record_count;
    else ++c->record_count;
}

/* variant Name { case, case(type), ... }: one of its cases, each with a value of one type or none (027's 2b). */
static void variant_declaration(Compiler *c) {
    Token name = expect_name(c, "variant name");
    declare(c, &name);
    if (c->module) fail_at(c, name.line, name.column, "a library declares no variant, for now");
    if (c->record_count == RECORDS) fail_at(c, name.line, name.column, "at most %d records are allowed, variants among them", RECORDS);
    Record *r = &c->records[c->record_count];
    memset(r, 0, sizeof(*r));
    memcpy(r->name, name.text, NAME);
    r->line = name.line, r->column = name.column;
    r->variant = r->typed = true;
    expect(c, "{", "'{' on the line of the variant name");
    separators(c);
    while (!is(c, "}")) {
        Token label = expect_name(c, "a case");
        if (reserved_name(label.text) || !strcmp(label.text, "case"))
            fail_at(c, label.line, label.column, "'%s' is reserved", label.text);
        for (unsigned i = 0; i < r->declared_count; ++i)
            if (!strcmp(r->declared[i].name, label.text)) fail_at(c, label.line, label.column, "duplicate case '%s'", label.text);
        if (r->declared_count == FIELDS_PER_RECORD - 1) fail_at(c, label.line, label.column, "a variant has at most 15 cases");
        unsigned index = r->declared_count++;
        memcpy(r->declared[index].name, label.text, NAME);
        r->declared[index].line = label.line, r->declared[index].column = label.column;
        if (is(c, "(")) {
            take(c);
            declared_type(c, r, index, true);
            expect(c, ")", "')'");
        }
        end_field(c);
    }
    take(c);
    if (!r->declared_count) fail_at(c, name.line, name.column, "a variant needs at least one case");
    ++c->record_count;
}

/* The record a list or a message's rows field holds: NAME, the app's, or
 * LIBRARY.NAME, a library's the app uses, which goes into the module.
 * Its name into name, its library (0: the app's) into library. */
static Token held_record(Compiler *c, char name[NAME], int *library) {
    Token first = expect_name(c, "a record");
    *library = 0;
    memcpy(name, first.text, NAME);
    if (!is(c, ".")) return first;
    take(c);
    Token second = expect_name(c, "the library's record");
    int used = used_library(c, c->module, first.text);
    if (used < 0) fail_at(c, first.line, first.column, "unknown library '%s'; write use %s first", first.text, first.text);
    int found = library_record(c, used, second.text);
    if (found < 0) fail_at(c, second.line, second.column, "library '%s' has no record '%s'", first.text, second.text);
    Record *r = &c->library_records[found];
    if (!r->used) r->used = true, r->used_line = second.line, r->used_column = second.column;
    *library = used;
    memcpy(name, second.text, NAME);
    return second;
}

static void state_declaration(Compiler *c, bool transient) {
    Token name = expect_name(c, "state name");
    declare(c, &name);
    if (c->states == STATES) fail_at(c, name.line, name.column, "at most %d states are allowed", STATES);
    unsigned index = c->states;
    memcpy(c->state_names[index], name.text, NAME);
    c->state_transient[index] = transient;
    c->state_line[index] = name.line;
    c->state_column[index] = name.column;
    if (is(c, ":")) {
        take(c);
        Token type = expect_name(c, "text, bytes, i64, cursor, mark, list or index");
        if (!strcmp(type.text, "bytes")) { /* state data: bytes: held as a text is, read and set whole (Pure 028) */
            c->state_types[index] = STATE_TEXT;
            c->state_bytes[index] = true;
            if (is(c, "=")) {
                take(c);
                c->state_text[index] = plain_string(c, "a bytes state");
            }
        } else if (!strcmp(type.text, "i64")) { /* state n: i64 = 0: exact, whole */
            c->state_types[index] = STATE_I64;
            if (is(c, "=")) {
                take(c);
                c->state_integer[index] = signed_integer(c, "an i64 state");
            }
        } else if (!strcmp(type.text, "text")) {
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
            if (type.text[0] == 'l') held_record(c, c->state_target[index], &c->state_library[index]);
            else memcpy(c->state_target[index], expect_name(c, "a list state").text, NAME);
            if (type.text[0] == 'l' && word(c, "by")) { /* keyed: items in key order, each key once */
                take(c);
                Token key = expect_name(c, "a field of the record");
                memcpy(c->state_key[index], key.text, NAME);
                c->state_key_line[index] = key.line;
                c->state_key_column[index] = key.column;
            }
        } else fail_at(c, type.line, type.column,
                       "state types are numbers, bools, i64, text, cursor or mark in a text, list of a record and index of "
                       "a list");
        ++c->states;
        return;
    }
    expect(c, "=", "'='");
    if (word(c, "true") || word(c, "false")) { /* state on = false: a bool */
        c->state_types[index] = STATE_BOOL;
        c->initial[index] = take(c).text[0] == 't';
    } else {
        c->state_types[index] = STATE_NUM;
        c->initial[index] = signed_number(c, "state initializer");
    }
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
    if (!is(c, "{")) { /* its list's items as they are: their fields its columns */
        c->view_whole[table] = true;
        return;
    }
    expect(c, "{", "'{' on the line of the table");
    separators(c);
    while (!is(c, "}")) {
        Token column = expect_name(c, "a column name");
        if (reserved_name(column.text)) fail_at(c, column.line, column.column, "'%s' is reserved", column.text);
        for (unsigned i = table + 1; i < c->views; ++i)
            if (!strcmp(c->view_names[i], column.text))
                fail_at(c, column.line, column.column, "duplicate column '%s'", column.text);
        if (c->views == VIEWS) fail_at(c, column.line, column.column, "at most %d views are allowed", VIEWS);
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
    if (is(c, "->")) { /* its results' types (027's 2c): number, i64, bool, text, bytes, a record or list of one */
        take(c);
        f->typed = true;
        do {
            if (f->result_count) take(c);
            Token type = expect_name(c, "a type: number, i64, bool, text, bytes, a record or list of one");
            bool list = !strcmp(type.text, "list");
            if (list) {
                expect_word(c, "of");
                type = expect_name(c, "a record");
            }
            if (f->result_count == PARAMS) fail_at(c, type.line, type.column, "a fn gives at most 8 values");
            memcpy(f->results[f->result_count].name, type.text, NAME);
            f->results[f->result_count].list = list;
            f->results[f->result_count].line = type.line, f->results[f->result_count].column = type.column;
            ++f->result_count;
        } while (is(c, ","));
    }
    if (is(c, "{")) { /* a block: statements, and returns (027's 2c) */
        f->block = block(c, 1);
        f->called = true;
    } else {
        expect(c, "=", f->typed ? "'=' or '{'" : "'=', '{' or '->'");
        f->body = expression(c, 0, false);
    }
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

/* A library declares fns, records and uses only: it has no state and sees none. */
static void library_declaration(Compiler *c) {
    Token kind = expect_name(c, "fn, record or use");
    if (!strcmp(kind.text, "fn")) fn_declaration(c);
    else if (!strcmp(kind.text, "record")) record_declaration(c);
    else if (!strcmp(kind.text, "use")) use_declaration(c);
    else fail_at(c, kind.line, kind.column, "a library declares only fn, record and use");
    end(c);
}

/* (name: type, ...) after a view's name: its arguments, the fields of an
 * input of their own named view.args, which a request gives (?name=value). */
static void view_arguments(Compiler *c, unsigned view, const Token *name) {
    take(c); /* ( */
    if (c->input_count == INPUTS) fail_at(c, name->line, name->column, "at most %d inputs are allowed (a view's arguments take one)", INPUTS);
    Input *in = &c->inputs[c->input_count];
    memset(in, 0, sizeof(*in));
    if (strlen(name->text) + 5 >= NAME)
        fail_at(c, name->line, name->column, "a view with arguments has a name of at most %d characters", NAME - 6);
    snprintf(in->name, NAME, "%s.args", name->text);
    in->kind = KIND_ARGUMENTS;
    in->view = view;
    newlines(c);
    while (!is(c, ")")) {
        Token argument = expect_name(c, "an argument name");
        if (reserved_name(argument.text)) fail_at(c, argument.line, argument.column, "'%s' is reserved", argument.text);
        if (!strcmp(argument.text, "from") || !strcmp(argument.text, "count"))
            fail_at(c, argument.line, argument.column, "from and count page a view's rows; name the argument otherwise");
        for (unsigned f = 0; f < in->fields; ++f)
            if (!strcmp(in->field_names[f], argument.text))
                fail_at(c, argument.line, argument.column, "duplicate argument '%s'", argument.text);
        expect(c, ":", "':'");
        Token type = expect_name(c, "u32, i64, number, bool, bytes or text");
        static const struct { const char *name; int type; } types[] = {
            {"u32", FIELD_U32}, {"i64", FIELD_I64}, {"number", FIELD_NUMBER}, {"bool", FIELD_BOOL}, {"bytes", FIELD_BYTES},
            {"text", FIELD_TEXT}};
        int field_type = 0;
        for (size_t t = 0; t < sizeof(types) / sizeof(*types); ++t)
            if (!strcmp(type.text, types[t].name)) field_type = types[t].type;
        if (!field_type) fail_at(c, type.line, type.column, "an argument is a u32, i64, number, bool, bytes or text");
        if (c->field_count == FIELDS) fail_at(c, argument.line, argument.column, "at most %d message fields are allowed", FIELDS);
        memcpy(in->field_names[in->fields], argument.text, NAME);
        in->field_line[in->fields] = argument.line;
        in->field_column[in->fields] = argument.column;
        in->field_types[in->fields++] = field_type;
        ++c->field_count;
        newlines(c);
        if (!is(c, ",")) break;
        take(c);
        newlines(c);
    }
    expect(c, ")", "')'");
    if (!in->fields) fail_at(c, name->line, name->column, "'%s()' takes no argument: write view %s = ...", name->text, name->text);
    c->view_arguments[view] = (int)++c->input_count;
}

static void declaration(Compiler *c) {
    Token kind = expect_name(c, "state, transient, record, input, output, view, fn, use, or on");
    if (!strcmp(kind.text, "fn")) {
        fn_declaration(c);
    } else if (!strcmp(kind.text, "use")) {
        use_declaration(c);
    } else if (!strcmp(kind.text, "state")) {
        state_declaration(c, false);
    } else if (!strcmp(kind.text, "transient")) {
        /* transient state x = 0: the app's while it runs, never stored */
        if (!word(c, "state")) fail_at(c, c->token.line, c->token.column, "expected 'state' after 'transient'");
        take(c);
        state_declaration(c, true);
    } else if (!strcmp(kind.text, "record")) {
        record_declaration(c);
    } else if (!strcmp(kind.text, "variant")) {
        variant_declaration(c);
    } else if (!strcmp(kind.text, "input")) {
        Token name = expect_name(c, "input name");
        declare(c, &name);
        if (c->input_count == INPUTS) fail_at(c, name.line, name.column, "at most %d inputs are allowed", INPUTS);
        expect(c, "=", "'='");
        Input *in = &c->inputs[c->input_count];
        memset(in, 0, sizeof(*in));
        memcpy(in->name, name.text, NAME);
        if (word(c, "message")) {
            take(c);
            in->kind = KIND_MESSAGE;
            ++c->input_count;
            if (word(c, "like")) {
                take(c);
                message_like(c, in);
            } else
                message_fields(c, in, "");
        } else if (word(c, "text")) {
            take(c);
            in->kind = KIND_TEXT;
            memcpy(in->field_names[0], "text", 5); /* its text, read as the input's name */
            in->field_types[0] = FIELD_TEXT;
            in->fields = 1;
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
    } else if (!strcmp(kind.text, "output")) {
        /* output ordered = message { ... }: what the app sends; its receiver is the host's wiring. */
        Token name = expect_name(c, "output name");
        declare(c, &name);
        if (c->input_count == INPUTS) fail_at(c, name.line, name.column, "at most %d inputs and outputs are allowed", INPUTS);
        unsigned outputs = 0; /* Pure's store names them in a byte's count */
        for (unsigned i = 0; i < c->input_count; ++i) outputs += c->inputs[i].kind == KIND_OUTPUT;
        if (outputs == OUTPUTS) fail_at(c, name.line, name.column, "at most %d outputs are allowed", OUTPUTS);
        expect(c, "=", "'='");
        Input *in = &c->inputs[c->input_count];
        memset(in, 0, sizeof(*in));
        memcpy(in->name, name.text, NAME);
        expect_word(c, "message");
        in->kind = KIND_OUTPUT;
        ++c->input_count;
        if (word(c, "like")) {
            take(c);
            message_like(c, in);
        } else
            message_fields(c, in, "");
    } else if (!strcmp(kind.text, "view")) {
        Token name = expect_name(c, "view name");
        declare(c, &name);
        if (c->views == VIEWS) fail_at(c, name.line, name.column, "at most %d views are allowed", VIEWS);
        unsigned index = c->views++;
        if (is(c, "(")) view_arguments(c, index, &name);
        memcpy(c->view_names[index], name.text, NAME);
        if (is(c, "{")) { /* view name(arguments) { statements ... return value }: a view that computes */
            Stmt *body = block(c, 0), *last = body;
            while (last && last->next) last = last->next;
            if (!last || last->kind != S_RETURN)
                fail_at(c, name.line, name.column, "a view's body ends with return and its value, as in return total");
            c->view_body[index] = body;
            c->view_value[index] = last->value;
        } else {
            expect(c, "=", "'='");
            if (!word(c, "each")) c->view_value[index] = expression(c, 0, false);
            else table_declaration(c, index);
        }
    } else if (!strcmp(kind.text, "on")) {
        if (c->handler_count == HANDLERS) fail_at(c, kind.line, kind.column, "at most %d handlers are allowed", HANDLERS);
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
    if (c->code_count == c->code_capacity) { /* a function's code grows */
        Op *grown = realloc(c->code, 2 * c->code_capacity * sizeof(*grown));
        if (!grown) fail_at(c, line, column, "out of memory");
        c->code = grown, c->code_capacity *= 2;
        c->instances[c->current].ops = grown;
    }
    c->code[c->code_count].op = op;
    c->code[c->code_count].arg = arg;
    c->code[c->code_count++].value = value;
}

/* An i64 constant: CONST with argument 2, its value the integer's 8 bytes. */
static void emit_integer(Compiler *c, int line, int column, int64_t value) {
    emit(c, line, column, CONST, 2, 0);
    c->code[c->code_count - 1].integer = value;
}

static const char *type_name(Type type) {
    static const char *const names[] = {"", "a number", "bytes", "text", "a cursor", "a distance", "an index", "a bool", "an i64",
                                        "a list", "a record", "several values", "no value"};
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

static int text_state(Compiler *c, const char *name, int line, int column) {
    int index = state_index(c, name);
    if (index >= 0 && c->state_types[index] == STATE_TEXT && c->state_bytes[index])
        fail_at(c, line, column, "'%s' is a bytes state: it is read and set whole, no cursor in it", name);
    if (index < 0 || c->state_types[index] != STATE_TEXT) {
        const char *kind = declared_kind(c, name);
        if (kind) fail_at(c, line, column, "'%s' is %s, not a text state", name, kind);
        fail_at(c, line, column, "unknown text state '%s'", name);
    }
    return index;
}

static int cursor_owner(const Compiler *c, int cursor) { return state_index(c, c->state_target[cursor]); }

/* The list state name[...] indexes (where: the index, or NULL). */
static int local_named(const Compiler *c, const char *name);
static int parameter(const Compiler *c, const char *name);

static int list_state(Compiler *c, const char *name, int line, int column, const Expr *where) {
    int index = state_index(c, name);
    if (index >= 0 && c->state_types[index] == STATE_LIST) {
        if (c->state_rows[index] && c->state_rows[index] - 1 != c->message)
            fail_at(c, line, column, "the rows of '%s' are only available in its handler", c->inputs[c->state_rows[index] - 1].name);
        return index;
    }
    (void)where;
    int local = local_named(c, name);
    if (parameter(c, name) >= 0 || (local >= 0 && c->locals[local].kind.type == LIST))
        fail_at(c, line, column, "'%s' is a list value (a copy, given or made): it is read by for, a fold, [i], .count, .first "
                "and .last, not changed; changes go to a let's list", name);
    const char *kind = declared_kind(c, name);
    if (kind) fail_at(c, line, column, "'%s' is %s, not a list", name, kind);
    fail_at(c, line, column, "unknown list '%s'", name);
}

static const Record *record_of(const Compiler *c, int list) {
    return &c->records[module_record(c, c->state_library[list], c->state_target[list])];
}

static unsigned record_field(Compiler *c, const Record *r, const char *field, int line, int column) {
    for (unsigned i = 0; i < r->fields; ++i) if (!strcmp(r->field_names[i], field)) return i;
    fail_at(c, line, column, "record '%s' has no field '%s'", record_shown(c, c->module, r), field);
}

/* A record field's kind: text, a bool or a number. */
static Type field_kind(const Record *r, unsigned field) {
    return r->field_kinds[field];
}

/* What reading a field gives: its kind, and a list's the record of its items. */
static Kind field_value(const Record *r, unsigned field) {
    return (Kind){r->field_kinds[field], r->field_kinds[field] == LIST ? r->field_record[field] : -1};
}

/* The field named exactly so, or -1. */
static int field_named(const Record *r, const char *name) {
    for (unsigned i = 0; i < r->fields; ++i) if (!strcmp(r->field_names[i], name)) return (int)i;
    return -1;
}

/* Whether a record holds fields under prefix (prefix.x: a record's or a variant's in a field). */
static bool holds_under(const Record *r, const char *prefix) {
    size_t n = strlen(prefix);
    for (unsigned i = 0; i < r->fields; ++i)
        if (!strncmp(r->field_names[i], prefix, n) && (r->field_names[i][n] == '.' || !r->field_names[i][n])) return true;
    return false;
}

/* The kind of a message field's value (a u32 is a number). */
static Type message_kind(int type) {
    return type == FIELD_TEXT ? TEXT : type == FIELD_BYTES ? BYTES : type == FIELD_BOOL ? BOOL : type == FIELD_I64 ? I64 : NUM;
}

static Kind value(Compiler *c, const Expr *e, bool handler, int height);

static void expect_kind(Compiler *c, const Expr *e, Kind actual, Type type) {
    const char *name = e->kind == E_MEMBER ? e->member : e->name;
    if (actual.type != type && actual.type == RECORD && c->records[actual.owner].variant && *name)
        fail_at(c, e->line, e->column, "'%s' holds a variant: match it, as in match %s { ... }", name, name);
    if (actual.type == LIST && type != LIST)
        fail_at(c, e->line, e->column, "expected %s, not a list: walk it (for x in ...) or read .count, .first or .last",
                type_name(type));
    if (actual.type != type) fail_at(c, e->line, e->column, "expected %s, not %s", type_name(type), type_name(actual.type));
}

static Kind wanted(Compiler *c, const Expr *e, bool handler, int height, Type want);

static Kind number(Compiler *c, const Expr *e, bool handler, int height) {
    Kind kind = wanted(c, e, handler, height, NUM); /* 0x... is a number here */
    expect_kind(c, e, kind, NUM);
    return kind;
}

/* A bool, where what (if, where, require, !, && or ||) takes one: a number
 * is not one, and the message says how to make one of it. */
static void truth(Compiler *c, const Expr *e, bool handler, int height, const char *what) {
    Kind kind = value(c, e, handler, height);
    if (kind.type == NUM) fail_at(c, e->line, e->column, "%s takes a bool, not a number: compare it, as in n != 0", what);
    expect_kind(c, e, kind, BOOL);
}

/* An integer literal (digits alone, or negated ones): a number, or an i64
 * where one is wanted. */
static bool loose(const Expr *e) { return e->kind == E_NUMBER && e->whole; }

/* The literal e, emitted at code[at] as a number, made the i64 wanted beside it. */
static void made_i64(Compiler *c, const Expr *e, unsigned at) {
    if (e->wide) fail_at(c, e->line, e->column, "the number is past an i64's range");
    c->code[at].arg = 2;
    c->code[at].integer = e->integer;
}

/* e where a value of type want goes (0: any): an integer literal is an i64
 * where an i64 is wanted, and so are the literals of an arithmetic or a
 * conditional expression that gives one. */
static Kind wanted(Compiler *c, const Expr *e, bool handler, int height, Type want) {
    if (want == I64 && loose(e)) {
        if (height + 1 > STACK) fail_at(c, e->line, e->column, "expression needs more than 64 stack values");
        emit(c, e->line, e->column, CONST, 2, 0);
        made_i64(c, e, c->code_count - 1);
        return (Kind){I64, -1};
    }
    c->want = want;
    return value(c, e, handler, height);
}

/* e where an i64 goes: a number is refused, and the message says how to convert it. */
static void integer(Compiler *c, const Expr *e, bool handler, int height) {
    Kind kind = wanted(c, e, handler, height, I64);
    if (kind.type == NUM)
        fail_at(c, e->line, e->column, e->kind == E_NUMBER ? "an i64 is a whole number; this one is not" :
                "expected an i64, not a number: convert it, as in i64(x)");
    expect_kind(c, e, kind, I64);
}

/* Integer literals alone, with arithmetic, negation, min, max and
 * conditional expressions of them: a number, or an i64 where one is wanted. */
static bool literal_only(const Expr *e) {
    if (loose(e)) return true;
    if (e->kind == E_IF) return literal_only(e->args[1]) && literal_only(e->args[2]); /* whatever its condition */
    bool arithmetic = (e->kind == E_BINARY && (e->op == ADD || e->op == SUB || e->op == MUL || e->op == DIV || e->op == FLOORDIV ||
                                               e->op == MOD) &&
                       e->args[1]->kind != E_UNIT) ||
                      (e->kind == E_CALL && (e->op == MIN || e->op == MAX)) || (e->kind == E_UNARY && e->op == NEG);
    for (unsigned i = 0; arithmetic && i < e->count; ++i) arithmetic = literal_only(e->args[i]);
    return arithmetic;
}

/* The type e has, compiled and taken back: what literals beside it take. */
static Kind meeting(Compiler *c, const Expr *x, bool handler, int height, Type other) {
    Kind kind = value(c, x, handler, height);
    if (x->kind == E_STRING && kind.type == TEXT && other == BYTES) {
        emit(c, x->line, x->column, BYTES_FROM, 0, 0);
        return (Kind){BYTES, -1};
    }
    return kind;
}

static Type probe(Compiler *c, const Expr *e, bool handler, int height) {
    unsigned code = c->code_count, constants = c->constants, joins = c->joins, instances = c->instance_count;
    size_t used = c->constant_used;
    Type type = value(c, e, handler, height).type;
    c->code_count = code, c->joins = joins;
    if (c->instance_count == instances) c->constants = constants, c->constant_used = used; /* a function made keeps its own */
    return type;
}

/* What an arithmetic operation's or a comparison's operands are wanted as,
 * before the left one is emitted: a number or an i64 when that is wanted of
 * the operation (want), else when the left one is literals alone the right
 * one's type (probed once: the right one is then compiled as wanted, so no
 * probe below it runs again). */
static Type left_hint(Compiler *c, const Expr *e, bool handler, int height, Type want) {
    if (want == I64 || want == NUM) return want;
    if (!literal_only(e->args[0]) || literal_only(e->args[1])) return 0;
    Type right = probe(c, e->args[1], handler, height + 1);
    return right == I64 || right == NUM ? right : 0;
}

/* The right operand of an arithmetic operation or a comparison whose left
 * one, of kind left, was emitted: both numbers or both i64s. Their type. */
static Type second(Compiler *c, const Expr *e, Kind left, bool handler, int height) {
    const Expr *a = e->args[0], *b = e->args[1];
    if (left.type == I64) {
        integer(c, b, handler, height + 1);
        return I64;
    }
    expect_kind(c, a, left, NUM);
    Kind right = wanted(c, b, handler, height + 1, NUM);
    if (right.type == I64)
        fail_at(c, b->line, b->column, "an i64 and a number do not mix: convert one, as in i64(x) or number(n)");
    expect_kind(c, b, right, NUM);
    return NUM;
}

static uint8_t i64_op(int op) {
    switch (op) {
    case ADD: return I64_ADD;
    case SUB: return I64_SUB;
    case MUL: return I64_MUL;
    case DIV: case FLOORDIV: return I64_DIV; /* (an i64's / refused: its quotient is //'s) */
    case MOD: return I64_MOD;
    case MIN: return I64_MIN;
    default: return I64_MAX;
    }
}

/* A conditional expression's branch, of the other branches' type (kind, once the first gives it). */
static void if_branch(Compiler *c, const Expr *e, bool handler, int height, Type hint, Kind *kind) {
    Kind got = wanted(c, e, handler, height, hint);
    if (kind->type && (got.type != kind->type || got.owner != kind->owner))
        fail_at(c, e->line, e->column, got.type != kind->type ? "the branches of if give %s and %s" : kind->type == CURSOR ?
                "the branches of if give cursors of different texts" : "the branches of if give indices of different lists",
                type_name(kind->type), type_name(got.type));
    *kind = got;
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
    unsigned at = c->code_count;
    Kind kind = value(c, e, handler, height);
    int key = c->list_key[list] - 1;
    if (kind.type != INDEX && key >= 0) {
        const Record *r = record_of(c, list);
        Type type = field_kind(r, (unsigned)key);
        if (type == I64 && kind.type == NUM && loose(e)) { /* xs[5] by an i64 key */
            made_i64(c, e, at);
            kind.type = I64;
        }
        if (kind.type != type)
            fail_at(c, e->line, e->column, "'%s' is keyed by %s, %s: expected %s or an index, not %s", c->state_names[list],
                    r->field_names[key], type_name(type), type_name(type), type_name(kind.type));
        emit(c, e->line, e->column, LIST_FIND, (uint32_t)list, 0);
        return;
    }
    if (kind.type == NUM || kind.type == I64) { /* xs[2]: the item at a position */
        if (kind.type == I64) emit(c, e->line, e->column, NUM_FROM, 0, 0);
        emit(c, e->line, e->column, INDEX_AT, (uint32_t)list, 0);
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

/* An item of list, its index on the stack: the value itself in a list of
 * i64 (number, bool, text), else the index. */
static Kind item_value(Compiler *c, const Expr *e, int list) {
    const Record *r = record_of(c, list);
    if (!r->scalar) return (Kind){INDEX, list};
    emit(c, e->line, e->column, ITEM_GET, (uint32_t)list, 0);
    return field_value(r, 0);
}


static Kind call_fn(Compiler *c, const Expr *e, int fn, bool handler, int height, Kind *kinds, unsigned given);
static bool variant_case(Compiler *c, const Expr *e, int *variant, unsigned *index, const Expr **payload);
static bool composite(const Record *r, const char *path);

/* A fn inlined: its arguments go on the stack once, the body reads them with
 * PICK, and SLIDE leaves only the result. e names the call (E_FN), or is the
 * name of a fn without parameters. */
static Kind inline_fn(Compiler *c, const Expr *e, int module, const char *name, bool handler, int height) {
    int index = fn_index(c, module, name);
    if (index < 0) {
        if (module != c->module) fail_at(c, e->line, e->column, "library '%s' has no fn '%s'", c->modules[module].name, name);
        const char *kind = declared_kind(c, name);
        if (kind) fail_at(c, e->line, e->column, "'%s' is %s, not a fn", name, kind);
        fail_at(c, e->line, e->column, "unknown fn '%s'", name);
    }
    Fn *f = &c->fns[index];
    unsigned count = e->kind == E_FN ? e->count : 0;
    if (count != f->param_count)
        fail_at(c, e->line, e->column, "%s expects %u argument%s", name, f->param_count, f->param_count == 1 ? "" : "s");
    if (f->active) fail_at(c, e->line, e->column, "fn '%s' calls itself", name);
    Scope scope = {.fn = index, .base = height, .line = e->line, .column = e->column};
    for (unsigned i = 0; i < count; ++i) {
        scope.kinds[i] = value(c, e->args[i], handler, height + (int)i);
        if (scope.kinds[i].type == RECORD || scope.kinds[i].type == TUPLE || scope.kinds[i].type == NOTHING) {
            if (scope.kinds[i].type != RECORD) fail_at(c, e->args[i]->line, e->args[i]->column, "an argument is one value");
            /* a record's value: the fn runs as a function, its arguments so far on the stack (027's 2c) */
            return call_fn(c, e, index, handler, height, scope.kinds, i + 1);
        }
    }
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
/* The arguments of the fn being inlined that an expression names (not where item, the fold's own item, hides one). */
static void reads_parameters(const Compiler *c, const Expr *e, const char *item, bool reads[PARAMS]) {
    for (; e; e = e->next) {
        if (e->kind == E_NAME && strcmp(e->name, item)) {
            const Fn *f = &c->fns[c->scope->fn];
            for (unsigned i = 0; i < f->param_count; ++i) if (!strcmp(f->params[i], e->name)) reads[i] = true;
        }
        for (unsigned i = 0; i < PARAMS; ++i) if (e->args[i]) reads_parameters(c, e->args[i], item, reads);
    }
}

static int parameter(const Compiler *c, const char *name) {
    if (!c->scope) return -1;
    const Fn *f = &c->fns[c->scope->fn];
    for (unsigned i = 0; i < f->param_count; ++i) if (!strcmp(f->params[i], name)) return (int)i;
    return -1;
}

/* A member chain on a message, as order.customer.name: the input and the
 * path after it (customer.name). False for anything else. */
static bool message_path(Compiler *c, const Expr *e, int *input, char path[NAME]) {
    if (e->kind != E_MEMBER) return false;
    const Expr *base = e->args[0];
    if (base->kind == E_NAME) {
        if (parameter(c, base->name) >= 0 || c->module) return false;
        *input = input_index(c, base->name);
        if (*input < 0 || c->inputs[*input].kind != KIND_MESSAGE) return false;
        snprintf(path, NAME, "%s", e->member);
        return true;
    }
    char inner[NAME];
    if (!message_path(c, base, input, inner)) return false;
    const Input *in = &c->inputs[*input];
    for (unsigned i = 0; i < in->fields; ++i)
        if (!strcmp(in->field_names[i], inner)) return false; /* a field's own member, as m.text.bytes */
    int length = snprintf(path, NAME, "%s.%s", inner, e->member);
    if (length < 0 || length >= NAME) snprintf(path, NAME, "%s", inner); /* too long: names nothing */
    return true;
}

/* The list a message's rows fill, when e names them (input.field): only in
 * that message's handler. -1 otherwise. */
static int rows_list(Compiler *c, const Expr *e) {
    int input;
    char path[NAME];
    if (!message_path(c, e, &input, path)) return -1;
    const Input *in = &c->inputs[input];
    for (unsigned f = 0; f < in->fields; ++f)
        if (in->field_types[f] == FIELD_ROWS && !strcmp(in->field_names[f], path)) {
            if (input != c->message) fail_at(c, e->line, e->column, "the rows of '%s' are only available in its handler", in->name);
            return in->field_list[f];
        }
    return -1;
}

/* A name of the app (a state or an input): not visible in a library
 * fn, and hidden by a parameter of the same name. */
static void app_name(Compiler *c, const Expr *e, const char *name) {
    if (c->module) fail_at(c, e->line, e->column, "'%s' is not a parameter; a library fn reads only its parameters", name);
    if (parameter(c, name) >= 0) fail_at(c, e->line, e->column, "'%s' is a parameter here", name);
}

/* The local named name in scope, or -1. */
/* A let of a value (or a for's count) in scope, by name, or -1; a let's list is a state (state_index). */
static int local_named(const Compiler *c, const char *name) {
    for (unsigned i = c->local_count; i-- > 0;)
        if (c->locals[i].list < 0 && !strcmp(c->locals[i].name, name)) return (int)i;
    return -1;
}

/* Any let in scope by name (a value's or a list's), or -1. */
static int let_named(const Compiler *c, const char *name) {
    for (unsigned i = c->local_count; i-- > 0;)
        if (!strcmp(c->locals[i].name, name)) return (int)i;
    return -1;
}

/* The for (walk or count) in whose block a name is its item or count, innermost first, or -1. */
static int loop_named(const Compiler *c, const char *name) {
    for (unsigned i = c->loop_count; i-- > 0;)
        if (!strcmp(c->loops[i].name, name)) return (int)i;
    return -1;
}

/* The innermost walk among the fors around, or -1. */
static int innermost_walk(const Compiler *c) {
    for (unsigned i = c->loop_count; i-- > 0;)
        if (c->loops[i].list >= 0) return (int)i;
    return -1;
}

/* Whether a list is walked by a for around: the innermost walk's (1), an outer walk's (2), or none (0). */
static int walked(const Compiler *c, int list) {
    int inner = innermost_walk(c);
    for (unsigned i = 0; inner >= 0 && i < c->loop_count; ++i)
        if (c->loops[i].list == list) return (int)i == inner ? 1 : 2;
    return 0;
}

/* Whether statements hold a walk (a for over a list), at any depth. */
static bool holds_walk(const Stmt *s) {
    for (; s; s = s->next)
        if (s->kind == S_FOR || holds_walk(s->then) || holds_walk(s->otherwise)) return true;
    return false;
}

static uint32_t local_arg(const Compiler *c, int local) {
    const Kind *kind = &c->locals[local].kind;
    return c->locals[local].slot | (kind->type == INDEX ? 1u << 8 | (uint32_t)kind->owner << 16 :
                                    kind->type == LIST ? (uint32_t)LOCAL_LIST << 8 | (uint32_t)kind->owner << 16 :
                                    kind->type == BOOL ? (uint32_t)LOCAL_BOOL << 8 : kind->type == I64 ? (uint32_t)LOCAL_I64 << 8 : 0);
}

/* Emit an expression; height is the stack height before it. */
/* Whether e cannot fail, as far as its form shows (conservative: any read that may find nothing, any arithmetic that
 * may pass its range, any conversion or call may). && and || compute their right side only where it decides, and
 * where it cannot fail they compute it anyway (one AND or OR). */
static Type probe(Compiler *c, const Expr *e, bool handler, int height);

/* A side of a comparison or a join: a string (a literal, or one with formats) where bytes are what it meets is its
 * UTF-8 bytes, as 1 is an i64 where i64s go; other values do not mix (bytes(t) and text(b) convert them). */
static Kind meeting(Compiler *c, const Expr *x, bool handler, int height, Type other);
static bool meets_bytes(Compiler *c, const Expr *string, const Expr *other, bool handler, int height) {
    return string->kind == E_STRING && probe(c, other, handler, height) == BYTES;
}

/* A list the code makes (027's 2b), an event's: a register a list in a record is bound to, to be read (bound; a new one
 * each time: each place reads its own), or the one empty list of a record (a LIST field's default). */
static int hidden_list(Compiler *c, int record, bool bound, int line, int column) {
    for (unsigned i = 0; !bound && i < c->states; ++i)
        if (c->state_hidden[i] && !c->state_bound[i] && c->state_record[i] == record) return (int)i;
    if (c->states == STATES)
        fail_at(c, line, column, "at most %d states are allowed, the lists the code reads in records among them", STATES);
    unsigned i = c->states++;
    snprintf(c->state_names[i], NAME, "%s.r%u", bound ? "bound" : "empty", i);
    c->state_types[i] = STATE_LIST;
    c->state_event[i] = c->state_hidden[i] = true;
    c->state_bound[i] = bound;
    c->state_record[i] = record;
    memcpy(c->state_target[i], c->records[record].name, NAME);
    c->state_library[i] = 0;
    c->list_key[i] = 0;
    c->state_line[i] = line, c->state_column[i] = column;
    return (int)i;
}

static Kind value(Compiler *c, const Expr *e, bool handler, int height);
static int walked_list(Compiler *c, const char *path, int line, int column);
static int walked_list_at(Compiler *c, const char *path, int line, int column, int height);
static void item_of(Compiler *c, const Expr *e, bool handler, int height, int list);
static Kind item_value(Compiler *c, const Expr *e, int list);
static int local_named(const Compiler *c, const char *name);
static int loop_named(const Compiler *c, const char *name);
static int parameter(const Compiler *c, const char *name);

static int alias_named(const Compiler *c, const char *name) {
    for (unsigned i = c->alias_count; i-- > 0;) if (!strcmp(c->aliases[i].name, name)) return (int)i;
    return -1;
}

/* The list value on the stack bound to a register of its own, to read and walk: the register. */
static int bind_list(Compiler *c, const Expr *e, Kind list) {
    int reg = hidden_list(c, list.owner, true, e->line, e->column);
    emit(c, e->line, e->column, LIST_BIND, (uint32_t)reg, 0);
    return reg;
}

/* name.field.field as an expression (a for's or a fold's list written as a path). */
static Expr *path_expr(Compiler *c, const char *path, int line, int column) {
    char part[NAME];
    size_t n = strcspn(path, ".");
    snprintf(part, sizeof(part), "%.*s", (int)n, path);
    Expr *e = node(c, E_NAME, line, column);
    memcpy(e->name, part, NAME);
    while (path[n] == '.') {
        path += n + 1;
        n = strcspn(path, ".");
        Expr *member = node(c, E_MEMBER, line, column);
        member->args[0] = e;
        member->count = 1;
        snprintf(member->member, NAME, "%.*s", (int)n, path);
        e = member;
    }
    return e;
}

/* An item and a field path in it (027's 2b): e an E_MEMBER chain whose root names an item (an alias's, a walk's or
 * a fold's item, an index let or state, the row, xs[i], a list's first or last, a list value's [i]). The item's index
 * is emitted and its list given; false, with nothing emitted, when the root names none. */
static bool item_path(Compiler *c, const Expr *e, bool handler, int height, int *list, char path[2 * NAME + 2]) {
    const char *members[16];
    unsigned count = 0;
    const Expr *root = e;
    while (root->kind == E_MEMBER && count < 16) members[count++] = root->member, root = root->args[0];
    if (root->kind == E_MEMBER) return false;
    char joined[2 * NAME + 2] = "";
    unsigned skip = 0;
    if (root->kind == E_NAME && parameter(c, root->name) < 0 && !c->module) {
        int alias = alias_named(c, root->name), loop = loop_named(c, root->name), local = local_named(c, root->name);
        int state = state_index(c, root->name);
        if (alias >= 0 && c->aliases[alias].prefix) {
            emit(c, e->line, e->column, LOCAL_LOAD, c->aliases[alias].slot | 1u << 8 | (uint32_t)c->aliases[alias].list << 16, 0);
            *list = c->aliases[alias].list;
            snprintf(joined, sizeof(joined), "%s", c->aliases[alias].path);
        } else if (alias >= 0) return false;
        else if (state >= 0 && c->state_types[state] == STATE_LIST && count >= 2 &&
                 (!strcmp(members[count - 1], "first") || !strcmp(members[count - 1], "last"))) {
            emit(c, e->line, e->column, members[count - 1][0] == 'f' ? INDEX_FIRST : INDEX_LAST, (uint32_t)state, 0);
            *list = state;
            skip = 1;
        } else if ((loop >= 0 && c->loops[loop].list >= 0) || (c->folding && !strcmp(root->name, c->fold_name)) ||
                   (local >= 0 && c->locals[local].kind.type == INDEX) || (state >= 0 && c->state_types[state] == STATE_INDEX) ||
                   (c->row >= 0 && !strcmp(root->name, c->view_row[c->row]))) {
            Kind kind = value(c, root, handler, height);
            if (kind.type != INDEX) return false;
            *list = kind.owner;
        } else return false;
    } else if (root->kind == E_ITEM && *root->member && root->count == 1) { /* xs[i].at.x */
        int state = state_index(c, root->name);
        if (state < 0 || c->state_types[state] != STATE_LIST) return false;
        item_of(c, root->args[0], handler, height, state);
        *list = state;
        snprintf(joined, sizeof(joined), "%s", root->member);
    } else if (root->kind == E_INDEX && root->count == 2) { /* b.items[i].at */
        Kind kind = value(c, root, handler, height);
        if (kind.type != INDEX) return false;
        *list = kind.owner;
    } else return false;
    for (unsigned k = count - skip; k-- > 0;) {
        size_t n = strlen(joined);
        snprintf(joined + n, sizeof(joined) - n, "%s%s", n ? "." : "", members[k]);
    }
    memcpy(path, joined, sizeof(joined));
    return true;
}

/* Whether two expressions are written the same (and so compute the same: expressions change nothing). */
static bool same_expr(const Expr *a, const Expr *b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind || a->op != b->op || a->count != b->count || strcmp(a->name, b->name) || strcmp(a->member, b->member) ||
        memcmp(&a->number, &b->number, sizeof(a->number)) || a->hex != b->hex || a->format != b->format || a->size != b->size ||
        (a->size && memcmp(a->bytes, b->bytes, a->size)))
        return false;
    for (unsigned i = 0; i < PARAMS; ++i) if (!same_expr(a->args[i], b->args[i])) return false;
    return same_expr(a->next, b->next);
}

/* Whether x is computed whenever in is (not in a branch of in's: an if's, or the right side of && and ||). */
static bool computed_in(const Expr *x, const Expr *in) {
    if (!in) return false;
    if (same_expr(x, in)) return true;
    if (in->kind == E_IF) return computed_in(x, in->args[0]);
    if (in->kind == E_BINARY && (in->op == AND || in->op == OR)) return computed_in(x, in->args[0]);
    if (in->kind == E_STRING) {
        for (const Expr *part = in->args[0]; part; part = part->next) if (computed_in(x, part)) return true;
        return false;
    }
    for (unsigned i = 0; i < in->count && i < PARAMS; ++i) if (computed_in(x, in->args[i])) return true;
    return false;
}

static bool faultless(Compiler *c, const Expr *e) {
    if (c->given && computed_in(e, c->given)) return true; /* the left side computed it: had it failed, so had the left */
    switch (e->kind) {
    case E_NUMBER: case E_BOOL: case E_TEXT: case E_DT: case E_NOW: return true;
    case E_NAME: /* a state, a let, a parameter, an argument, an item a walk or a fold runs for; not a fn's value */
        return parameter(c, e->name) >= 0 || fn_index(c, c->module, e->name) < 0;
    case E_STRING:
        for (const Expr *part = e->args[0]; part; part = part->next)
            if (!faultless(c, part) || (part->format & 0x7FFFFFFFu) >> FORMAT_FORM & 7) return false; /* a form may refuse */
        return true;
    case E_MEMBER: { /* a message's field, a list's count, a text's length; a field of the item a walk or fold runs for */
        const Expr *base = e->args[0];
        if (!strcmp(e->member, "count") || !strcmp(e->member, "chars") || !strcmp(e->member, "lines") ||
            !strcmp(e->member, "bytes") || !strcmp(e->member, "position"))
            return faultless(c, base);
        if (base->kind != E_NAME) return false;
        int input;
        char path[NAME];
        if (message_path(c, e, &input, path)) return true;
        return (c->folding && !strcmp(base->name, c->fold_name)) || loop_named(c, base->name) >= 0 ||
               (c->row >= 0 && !strcmp(base->name, c->view_row[c->row]));
    }
    case E_UNARY: return e->op == NOT && faultless(c, e->args[0]);
    case E_BINARY:
        if (e->op >= LT && e->op <= NE) return faultless(c, e->args[0]) && faultless(c, e->args[1]);
        if (e->op == AND || e->op == OR || e->op == BIT_AND || e->op == BIT_OR || e->op == BIT_XOR)
            return faultless(c, e->args[0]) && faultless(c, e->args[1]);
        if ((e->op == ADD || e->op == SUB) && e->args[1]->kind != E_UNIT) {
            /* a finite number and a literal below 2^969 cannot sum past the largest finite one (it rounds to it) */
            for (unsigned k = 0; k < 2; ++k) {
                const Expr *literal = e->args[k], *other = e->args[1 - k];
                if (literal->kind == E_NUMBER && !literal->hex && fabs(literal->number) < 0x1p969 && faultless(c, other) &&
                    probe(c, other, c->handling, 0) == NUM)
                    return true;
            }
        }
        return false; /* arithmetic may pass its range, a shift its count */
    case E_CALL: return (e->op == MIN || e->op == MAX || e->op == FLOOR) && faultless(c, e->args[0]) &&
                        (e->count < 2 || faultless(c, e->args[1]));
    case E_IF: return faultless(c, e->args[0]) && faultless(c, e->args[1]) && faultless(c, e->args[2]);
    default: return false;
    }
}

/* ---- Fns run as Pure's functions, and records' values (027's 2c) */

static void statements(Compiler *c, const Stmt *s);
static int typed_record(Compiler *c, const char *type, bool list, int line, int column);
static void record_values_of(Compiler *c, const Expr *e, const Record *r, const char *label, const Expr **values, int height);
static void record_fields_of(Compiler *c, const Expr *e, const Record *r, const Expr **values, int height);
static int scalar_record(Compiler *c, const char *name, int line, int column);

/* The stack values a value of a kind takes: a record's fields, else one. */
static unsigned cells(const Compiler *c, Kind kind) { return kind.type == RECORD ? c->records[kind.owner].fields : 1; }

static bool same_kind(Kind a, Kind b) {
    return a.type == b.type && (a.owner == b.owner || (a.type != RECORD && a.type != LIST && a.type != INDEX && a.type != CURSOR));
}

/* A kind as written: the name of a type (a record or a variant its value), or list of a record. */
static Kind written_kind(Compiler *c, int module, const char *name, bool list, int line, int column) {
    static const struct { const char *name; Type type; } scalars[] = {{"number", NUM}, {"i64", I64}, {"bool", BOOL}, {"text", TEXT},
                                                                       {"bytes", BYTES}};
    for (unsigned k = 0; !list && k < sizeof(scalars) / sizeof(*scalars); ++k)
        if (!strcmp(name, scalars[k].name)) return (Kind){scalars[k].type, -1};
    int record = module_record(c, module, name);
    if (record < 0 && list) record = scalar_record(c, name, line, column);
    if (record < 0) fail_at(c, line, column, "unknown type '%s': number, i64, bool, text, bytes, a record or list of one", name);
    return (Kind){list ? LIST : RECORD, record};
}

/* The record or variant a record holds at a path of its fields as written (at, a.b, or a variant's case pt), or -1. */
static int sub_record(Compiler *c, int record, const char *path) {
    const Record *r = &c->records[record];
    size_t n = strcspn(path, ".");
    for (unsigned i = 0; i < r->declared_count; ++i)
        if (strlen(r->declared[i].name) == n && !strncmp(r->declared[i].name, path, n) && *r->declared[i].type &&
            !r->declared[i].list) {
            int inner = typed_record(c, r->declared[i].type, false, r->declared[i].line, r->declared[i].column);
            return path[n] ? sub_record(c, inner, path + n + 1) : inner;
        }
    return -1;
}

/* A slot of the block for a let of bytes or text (Pure's: 0-15 bytes, 16-31 texts). */
static unsigned held_slot_at(Compiler *c, Type type, int line, int column) {
    unsigned *count = type == BYTES ? &c->held_bytes : &c->held_texts;
    if (*count == HELD) fail_at(c, line, column, "at most %d lets of %s are allowed in a handler", HELD, type_name(type));
    return (type == TEXT ? HELD_TEXT : 0) + (*count)++;
}

/* A name for a value of a kind in the block's scope: a let's slot, held by name (bytes, text), or a record's: the name
 * (its record its kind, no slot) and name.field for each field. */
static void name_value(Compiler *c, const char *name, Kind kind, int line, int column) {
    if (c->local_count == LOCALS) fail_at(c, line, column, "at most %d lets are allowed in a handler", LOCALS);
    unsigned local = c->local_count++;
    memset(&c->locals[local], 0, sizeof(c->locals[local]));
    snprintf(c->locals[local].name, NAME, "%s", name);
    c->locals[local].kind = kind;
    c->locals[local].list = -1;
    if (kind.type == RECORD) {
        const Record *r = &c->records[kind.owner];
        for (unsigned k = 0; k < r->fields; ++k) {
            char field[2 * NAME + 2];
            snprintf(field, sizeof(field), "%s.%s", name, r->field_names[k]);
            if (strlen(field) >= NAME) fail_at(c, line, column, "'%s' passes 31 bytes: give '%s' a shorter name", field, name);
            name_value(c, field, field_value(r, k), line, column);
            c->locals[c->local_count - 1].variant = r->field_variant[k] != 0;
        }
    } else if (kind.type == BYTES || kind.type == TEXT) c->locals[local].slot = held_slot_at(c, kind.type, line, column);
    else if (kind.type == NUM || kind.type == BOOL || kind.type == I64 || kind.type == LIST || kind.type == INDEX) {
        if (c->local_slots == LOCALS) fail_at(c, line, column, "at most %d lets are allowed in a handler", LOCALS);
        c->locals[local].slot = c->local_slots++;
    } else fail_at(c, line, column, "a name holds a number, a bool, an i64, bytes, text, a list or a record, not %s", type_name(kind.type));
}

/* The local that holds field k of record r at base: a name's own record's (root: base.field), or a record under a
 * name's (the variant's case is base itself, its other fields base.field). */
static void field_path(const Record *r, unsigned k, const char *base, bool root, char out[2 * NAME + 2]) {
    if (!root && r->field_variant[k] && !strcmp(r->field_names[k], "case")) snprintf(out, 2 * NAME + 2, "%s", base);
    else snprintf(out, 2 * NAME + 2, "%s.%s", base, r->field_names[k]);
}

static void load_named(Compiler *c, int local, int line, int column) {
    if (c->locals[local].kind.type == BYTES || c->locals[local].kind.type == TEXT)
        emit(c, line, column, BYTES_LOAD, c->locals[local].slot, 0);
    else emit(c, line, column, LOCAL_LOAD, local_arg(c, local), 0);
}

static void store_named(Compiler *c, int local, int line, int column) {
    if (c->locals[local].alias) fail_at(c, line, column, "'%s' names what a case holds; it is not assigned", c->locals[local].name);
    if (c->locals[local].kind.type == BYTES || c->locals[local].kind.type == TEXT)
        emit(c, line, column, BYTES_STORE, c->locals[local].slot, 0);
    else emit(c, line, column, LOCAL_STORE, local_arg(c, local), 0);
}

/* A record's value at base onto the stack, field by field (root: a name's own record), or taken off it into them. */
static void push_record(Compiler *c, const char *base, bool root, int record, int line, int column) {
    const Record *r = &c->records[record];
    for (unsigned k = 0; k < r->fields; ++k) {
        char path[2 * NAME + 2];
        field_path(r, k, base, root, path);
        load_named(c, local_named(c, path), line, column);
    }
}
static void pop_record(Compiler *c, const char *base, bool root, int record, int line, int column) {
    const Record *r = &c->records[record];
    for (unsigned k = r->fields; k-- > 0;) {
        char path[2 * NAME + 2];
        field_path(r, k, base, root, path);
        store_named(c, local_named(c, path), line, column);
    }
}

/* A value off the stack into a name of the block (a record's into its fields'). */
static void pop_named(Compiler *c, int local, int line, int column) {
    if (c->locals[local].kind.type == RECORD) pop_record(c, c->locals[local].name, true, c->locals[local].kind.owner, line, column);
    else store_named(c, local, line, column);
}

/* A path down a record's value a name holds (a let's, a parameter's, a match's name for a case's value): the path and
 * the name's local, or -1 when e is none. */
static int binding_path(Compiler *c, const Expr *e, char path[2 * NAME + 2]) {
    if (e->kind == E_NAME) {
        int local = c->scope ? -1 : local_named(c, e->name);
        if (local < 0 || c->locals[local].kind.type != RECORD) return -1;
        snprintf(path, 2 * NAME + 2, "%s", e->name);
        return local;
    }
    if (e->kind != E_MEMBER) return -1;
    int local = binding_path(c, e->args[0], path);
    size_t n = strlen(path);
    if (local >= 0) snprintf(path + n, 2 * NAME + 2 - n, ".%s", e->member);
    return local;
}

/* The item whose index is on the stack as its record's value (or its record's at a path, prefix): its fields, by way
 * of a local. A function has its own lists, so an item goes to one as a value. */
static Kind item_record(Compiler *c, const Expr *e, int list, const char *prefix, int record) {
    if (c->local_slots == LOCALS) fail_at(c, e->line, e->column, "at most %d lets are allowed in a handler", LOCALS);
    uint32_t local = c->local_slots++ | 1u << 8 | (uint32_t)list << 16;
    emit(c, e->line, e->column, LOCAL_STORE, local, 0);
    const Record *r = &c->records[record], *in = record_of(c, list);
    for (unsigned k = 0; k < r->fields; ++k) {
        char path[2 * NAME + 2];
        if (*prefix) field_path(r, k, prefix, false, path);
        else snprintf(path, sizeof(path), "%s", r->field_names[k]);
        emit(c, e->line, e->column, LOCAL_LOAD, local, 0);
        emit(c, e->line, e->column, ITEM_GET, (uint32_t)list | (uint32_t)field_named(in, path) << 16, 0);
    }
    return (Kind){RECORD, record};
}

/* A value as a function takes it or gives it: an item's as its record's (its index on the stack). */
static Kind as_value(Compiler *c, const Expr *e, Kind kind) {
    if (kind.type == INDEX) {
        int record = (int)(record_of(c, kind.owner) - c->records);
        return item_record(c, e, kind.owner, "", record);
    }
    if (kind.type == CURSOR || kind.type == UNIT || kind.type == NOTHING)
        fail_at(c, e->line, e->column, "%s does not go to a fn's call or come back from one", type_name(kind.type));
    return kind;
}

/* Whether code that follows a statement in its block never runs (it returns or breaks on every way), and whether
 * statements return on every way through them. */
static bool stops(const Stmt *s, bool breaks);
static bool stops_one(const Stmt *s, bool breaks) {
    switch (s->kind) {
    case S_RETURN: return true;
    case S_BREAK: return breaks;
    case S_IF: return s->otherwise && stops(s->then, breaks) && stops(s->otherwise, breaks);
    case S_MATCH:
        for (const Stmt *arm = s->then; arm; arm = arm->next) if (!stops(arm->then, breaks)) return false;
        return s->then != NULL; /* a match is exhaustive (its compiling says so) */
    default: return false;
    }
}
static bool stops(const Stmt *s, bool breaks) {
    if (!s) return false;
    while (s->next) s = s->next;
    return stops_one(s, breaks);
}

static bool gives_values(const Stmt *s) {
    for (; s; s = s->next)
        if ((s->kind == S_RETURN && s->value) || gives_values(s->then) || gives_values(s->otherwise)) return true;
    return false;
}

/* The fns a fn's body names (calls, or names a fn without parameters), a bit each. */
static void named_fns(const Compiler *c, int module, const Expr *e, uint64_t *out) {
    for (; e; e = e->next) {
        if ((e->kind == E_FN && !*e->member) || e->kind == E_NAME) {
            int f = fn_index(c, module, e->name);
            if (f >= 0) *out |= UINT64_C(1) << f;
        }
        if (e->kind == E_FN && *e->member) {
            int library = used_library(c, module, e->member), f = library >= 0 ? fn_index(c, library, e->name) : -1;
            if (f >= 0) *out |= UINT64_C(1) << f;
        }
        for (unsigned i = 0; i < PARAMS; ++i) if (e->args[i]) named_fns(c, module, e->args[i], out);
    }
}
static void named_by(const Compiler *c, int module, const Stmt *s, uint64_t *out) {
    for (; s; s = s->next) {
        const Expr *parts[] = {s->value, s->first, s->last, s->low, s->high};
        for (unsigned i = 0; i < 5; ++i) named_fns(c, module, parts[i], out);
        named_by(c, module, s->then, out);
        named_by(c, module, s->otherwise, out);
    }
}

/* Which fns run as functions: those with a block, and those a cycle of calls goes through. */
static void called_fns(Compiler *c) {
    uint64_t reach[FNS] = {0};
    for (unsigned i = 0; i < c->fn_count; ++i) {
        named_fns(c, c->fns[i].module, c->fns[i].body, &reach[i]);
        named_by(c, c->fns[i].module, c->fns[i].block, &reach[i]);
    }
    for (unsigned k = 0; k < c->fn_count; ++k)
        for (unsigned i = 0; i < c->fn_count; ++i)
            if (reach[i] >> k & 1) reach[i] |= reach[k];
    for (unsigned i = 0; i < c->fn_count; ++i) c->fns[i].called = c->fns[i].block || (reach[i] >> i & 1);
}

/* What making a function's code puts aside of what was being made. */
typedef struct {
    int message, arguments, row, fold_list, module, current;
    bool folding, filtering, handling, has_returned;
    char fold_name[NAME];
    Scope *fold_scope, *scope;
    const Expr *given;
    unsigned local_count, local_slots, held_bytes, held_texts, alias_count, loop_count, nested, joins, code_count, code_capacity;
    Type want;
    Kind returned;
    Op *code;
} Aside;

static void put_aside(Compiler *c, Aside *a, void *locals, void *aliases, void *loops) {
    *a = (Aside){c->message, c->arguments, c->row, c->fold_list, c->module, c->current, c->folding, c->filtering, c->handling,
                 c->has_returned, {0}, c->fold_scope, c->scope, c->given, c->local_count, c->local_slots, c->held_bytes,
                 c->held_texts, c->alias_count, c->loop_count, c->nested, c->joins, c->code_count, c->code_capacity, c->want,
                 c->returned, c->code};
    memcpy(a->fold_name, c->fold_name, NAME);
    memcpy(locals, c->locals, sizeof(c->locals));
    memcpy(aliases, c->aliases, sizeof(c->aliases));
    memcpy(loops, c->loops, sizeof(c->loops));
}

static void take_back(Compiler *c, const Aside *a, const void *locals, const void *aliases, const void *loops) {
    c->message = a->message, c->arguments = a->arguments, c->row = a->row, c->fold_list = a->fold_list, c->module = a->module;
    c->current = a->current, c->folding = a->folding, c->filtering = a->filtering, c->handling = a->handling;
    c->has_returned = a->has_returned, c->fold_scope = a->fold_scope, c->scope = a->scope, c->given = a->given;
    c->local_count = a->local_count, c->local_slots = a->local_slots, c->held_bytes = a->held_bytes, c->held_texts = a->held_texts;
    c->alias_count = a->alias_count, c->loop_count = a->loop_count, c->nested = a->nested, c->joins = a->joins;
    c->code_count = a->code_count, c->code_capacity = a->code_capacity, c->want = a->want, c->returned = a->returned;
    c->code = a->code;
    memcpy(c->fold_name, a->fold_name, NAME);
    memcpy(c->locals, locals, sizeof(c->locals));
    memcpy(c->aliases, aliases, sizeof(c->aliases));
    memcpy(c->loops, loops, sizeof(c->loops));
}

/* An instance's code, made in the functions' code: its parameters' values taken off the stack into names, its block
 * (or its expression, returned), and a return at its end when its way may reach it (giving nothing). */
static void make_instance(Compiler *c, int index) {
    static __typeof__(c->locals) locals[INSTANCES];
    static __typeof__(c->aliases) aliases[INSTANCES];
    static __typeof__(c->loops) loops[INSTANCES];
    static unsigned depth;
    Aside aside;
    put_aside(c, &aside, locals[depth], aliases[depth], loops[depth]);
    ++depth;
    Instance *in = &c->instances[index];
    const Fn *f = &c->fns[in->fn];
    if (!(in->ops = malloc(256 * sizeof(*in->ops)))) fail_at(c, f->line, f->column, "out of memory");
    c->code = in->ops, c->code_count = 0, c->code_capacity = 256;
    c->module = f->module, c->current = index, c->scope = NULL, c->handling = true, c->has_returned = false;
    c->message = c->arguments = c->row = c->fold_list = -1;
    c->folding = c->filtering = false, c->fold_scope = NULL, c->given = NULL, c->want = 0, c->joins = 0;
    c->local_count = c->local_slots = c->held_bytes = c->held_texts = c->alias_count = c->loop_count = c->nested = 0;
    for (unsigned i = 0; i < in->param_count; ++i) name_value(c, f->params[i], in->params[i], f->line, f->column);
    for (unsigned i = in->param_count; i-- > 0;) pop_named(c, local_named(c, f->params[i]), f->line, f->column);
    if (f->block) {
        statements(c, f->block);
        if (!stops(f->block, false)) {
            if (in->known && in->result_count)
                fail_at(c, f->line, f->column, "fn '%s' may end without giving its value: return it at the end", f->name);
            in->known = true, in->result_count = 0;
            emit(c, f->line, f->column, RETURN, (uint32_t)index, 0);
        }
    } else { /* an expression that calls itself, returned */
        Kind kind = as_value(c, f->body, value(c, f->body, true, 0));
        if (in->known && (in->result_count != 1 || !same_kind(in->results[0], kind)))
            fail_at(c, f->body->line, f->body->column, "fn '%s' gives %s, not %s", f->name,
                    in->result_count == 1 ? type_name(in->results[0].type) : "several values", type_name(kind.type));
        in->results[0] = kind, in->result_count = 1, in->known = true;
        emit(c, f->line, f->column, RETURN, (uint32_t)index, 0);
    }
    in->count = c->code_count, in->capacity = c->code_capacity;
    --depth;
    take_back(c, &aside, locals[depth], aliases[depth], loops[depth]);
}

/* The instance of fn for arguments of these kinds: one made already, or a new one (made now). */
static int instance_of(Compiler *c, int fn, const Kind *kinds, unsigned count, const Expr *at) {
    for (unsigned i = 0; i < c->instance_count; ++i) {
        const Instance *in = &c->instances[i];
        bool same = in->fn == fn && in->param_count == count;
        for (unsigned k = 0; same && k < count; ++k) same = same_kind(in->params[k], kinds[k]);
        if (same) return (int)i;
    }
    if (c->instance_count == INSTANCES)
        fail_at(c, at->line, at->column, "at most %d functions (a fn's for each kinds of its arguments) are allowed", INSTANCES);
    int index = (int)c->instance_count++;
    Instance *in = &c->instances[index];
    const Fn *f = &c->fns[fn];
    memset(in, 0, sizeof(*in));
    in->fn = fn, in->param_count = count;
    memcpy(in->params, kinds, count * sizeof(*kinds));
    if (f->typed) {
        for (unsigned k = 0; k < f->result_count; ++k)
            in->results[k] = written_kind(c, f->module, f->results[k].name, f->results[k].list, f->results[k].line, f->results[k].column);
        in->result_count = f->result_count, in->known = true;
    } else if (f->block && !gives_values(f->block)) in->known = true; /* it gives nothing */
    make_instance(c, index);
    return index;
}

/* A call of a fn run as a function: its arguments onto the stack (an item's as its record's value), the CALL, and
 * what it gives (a value, several: TUPLE, or none). Arguments already given (count of them, kinds) are on the stack. */
static Kind call_fn(Compiler *c, const Expr *e, int fn, bool handler, int height, Kind *kinds, unsigned given) {
    const Fn *f = &c->fns[fn];
    unsigned count = e->kind == E_FN ? e->count : 0;
    if (count != f->param_count)
        fail_at(c, e->line, e->column, "%s expects %u argument%s", f->name, f->param_count, f->param_count == 1 ? "" : "s");
    if (c->folding) fail_at(c, e->line, e->column, "count, sum, max and min call no fn with a block, nor one that calls itself");
    if (!handler && c->current < 0)
        fail_at(c, e->line, e->column, "a view calls no fn with a block, nor one that calls itself: handlers do ('%s')", f->name);
    int h = height;
    Kind own[PARAMS];
    if (!kinds) kinds = own;
    for (unsigned i = 0; i < given; ++i) h += (int)cells(c, kinds[i]);
    for (unsigned i = given; i < count; ++i) {
        kinds[i] = as_value(c, e->args[i], value(c, e->args[i], handler, h));
        h += (int)cells(c, kinds[i]);
        if (h > STACK) fail_at(c, e->line, e->column, "expression needs more than 64 stack values");
    }
    int index = instance_of(c, fn, kinds, count, e);
    const Instance *in = &c->instances[index];
    if (!in->known)
        fail_at(c, e->line, e->column, "fn '%s' calls itself before a return gives its value: write its type, as in fn %s(...) -> type",
                f->name, f->name);
    emit(c, e->line, e->column, CALL, (uint32_t)index, 0);
    unsigned gives = 0;
    for (unsigned k = 0; k < in->result_count; ++k) gives += cells(c, in->results[k]);
    if (height + (int)gives > STACK) fail_at(c, e->line, e->column, "expression needs more than 64 stack values");
    return in->result_count == 0 ? (Kind){NOTHING, -1} : in->result_count == 1 ? in->results[0] : (Kind){TUPLE, index};
}

/* A record's or a variant's value written out (R { ... }, V.case(x), V.case) onto the stack: its fields in order. */
static Kind made_record(Compiler *c, const Expr *e, int record, int height) {
    const Record *r = &c->records[record];
    const Expr *values[FIELDS_PER_RECORD];
    record_values_of(c, e, r, r->name, values, height);
    record_fields_of(c, e, r, values, height);
    return (Kind){RECORD, record};
}

/* A name's record's value, or a value under it (a field, a record's or variant's value at a path), onto the stack. */
static Kind binding_value(Compiler *c, const Expr *e, int binding, char *path) {
    int local = local_named(c, path);
    size_t n = strlen(path);
    if (local >= 0 && c->locals[local].kind.type == RECORD) {
        push_record(c, path, true, c->locals[local].kind.owner, e->line, e->column);
        return c->locals[local].kind;
    }
    if (local >= 0) {
        if (c->locals[local].variant && !e->raw)
            fail_at(c, e->line, e->column, "'%s' holds a variant: match it, as in match %s { ... }", path, path);
        load_named(c, local, e->line, e->column);
        return c->locals[local].kind;
    }
    if (n > 5 && !strcmp(path + n - 5, ".case")) { /* a variant's case under it, read as its own field */
        path[n - 5] = 0;
        local = local_named(c, path);
        if (local >= 0 && c->locals[local].variant) {
            load_named(c, local, e->line, e->column);
            return c->locals[local].kind;
        }
        path[n - 5] = '.';
    }
    int record = sub_record(c, c->locals[binding].kind.owner, path + strlen(c->locals[binding].name) + 1);
    if (record < 0) return (Kind){0, -1}; /* none here: what the expression reads of a field (name.text.bytes) */
    push_record(c, path, false, record, e->line, e->column);
    return (Kind){RECORD, record};
}

static Kind value(Compiler *c, const Expr *e, bool handler, int height) {
    Type want = c->want; /* set by wanted() for this expression alone */
    c->want = 0;
    if (height + 1 > STACK) fail_at(c, e->line, e->column, "expression needs more than 64 stack values");
    switch (e->kind) {
    case E_NUMBER:
        if (e->hex && want == NUM) { /* 0x... where a number goes (140's UAX #14 friction): that number, exactly */
            if (e->wide || e->integer > INT64_C(9007199254740992))
                fail_at(c, e->line, e->column, "the number is past 2^53, where numbers stop being exact: write it as an i64");
            emit(c, e->line, e->column, CONST, 0, (double)e->integer);
            return (Kind){NUM, -1};
        }
        if (e->hex) { /* 0x...: an i64 */
            emit(c, e->line, e->column, CONST, 2, 0);
            made_i64(c, e, c->code_count - 1);
            return (Kind){I64, -1};
        }
        emit(c, e->line, e->column, CONST, 0, e->number);
        return (Kind){NUM, -1};
    case E_BOOL: emit(c, e->line, e->column, CONST, 1, e->number); return (Kind){BOOL, -1};
    case E_DT: case E_NOW: {
        const char *name = e->kind == E_DT ? "dt" : "now";
        if (c->folding) fail_at(c, e->line, e->column, "%s is not available in count, sum, max or min", name);
        if (!handler)
            fail_at(c, e->line, e->column, e->kind == E_DT ? "dt is only available in handlers" :
                    "now is only available in handlers; a view shows a state that a timer or an event sets");
        if (e->kind == E_DT && c->message >= 0 && c->inputs[c->message].kind == KIND_FRAME)
            emit(c, e->line, e->column, FIELD, 0, 0); /* the frame's dt */
        else if (e->kind == E_DT) emit(c, e->line, e->column, CONST, 0, 0); /* no frame: none passed */
        else emit(c, e->line, e->column, NOW, 0, 0);
        return (Kind){NUM, -1};
    }
    case E_TEXT: emit(c, e->line, e->column, TEXT_CONST, constant(c, e), 0); return (Kind){TEXT, -1};
    case E_STRING: {
        int pieces = 0;
        for (const Expr *part = e->args[0]; part; part = part->next) {
            Kind kind = value(c, part, handler, height + (pieces > 0));
            uint32_t format = part->format & 0x7FFFFFFFu, form = format >> FORMAT_FORM & 7;
            if (part->format && kind.type != NUM && kind.type != BOOL && kind.type != I64)
                fail_at(c, part->line, part->column, "a format writes a number, a bool or an i64, not %s", type_name(kind.type));
            if (kind.type == BOOL && form) fail_at(c, part->line, part->column, "a bool's format is a width alone");
            if (kind.type == I64 && form == FORM_FIXED)
                fail_at(c, part->line, part->column, "f writes a number; an i64 in cents is written by .2d (1234 is 12.34)");
            if (kind.type == NUM || kind.type == BOOL || kind.type == I64)
                emit(c, part->line, part->column, NUM_TEXT, (kind.type == BOOL ? 1u : kind.type == I64 ? 2u : 0u) | format, 0);
            else if (kind.type != TEXT)
                fail_at(c, part->line, part->column, "a string shows numbers, bools, i64s and text, not %s", type_name(kind.type));
            if (pieces++) emit(c, part->line, part->column, CONCAT, 0, 0);
        }
        return (Kind){TEXT, -1};
    }
    case E_NAME: {
        if (c->folding && c->scope == c->fold_scope && !strcmp(e->name, c->fold_name)) {
            emit(c, e->line, e->column, ROW, (uint32_t)c->fold_list, 0); /* the item a fold's block runs for */
            return item_value(c, e, c->fold_list);
        }
        int loop = loop_named(c, e->name);
        if (loop >= 0 && c->loops[loop].list >= 0 && c->scope == c->loops[loop].scope) {
            /* the item a for's block runs for; in a count, sum, max or min inside it, still there; an outer
             * walk's, inside the walks within it, kept in a local */
            int list = c->loops[loop].list;
            if (loop == innermost_walk(c)) emit(c, e->line, e->column, c->folding ? OUTER_ROW : ROW, (uint32_t)list, 0);
            else emit(c, e->line, e->column, LOCAL_LOAD, (uint32_t)c->loops[loop].item | 1u << 8 | (uint32_t)list << 16, 0);
            return item_value(c, e, list);
        }
        int alias = c->scope ? -1 : alias_named(c, e->name);
        if (alias >= 0) { /* a match's binding: its item's field */
            int list = c->aliases[alias].list;
            if (c->aliases[alias].prefix) { /* a record's or a variant's value under the item (027's 2c) */
                const Record *r = record_of(c, list);
                int sub = sub_record(c, (int)(r - c->records), c->aliases[alias].path);
                if (sub < 0)
                    fail_at(c, e->line, e->column, "'%s' holds a record or a variant: read its fields (%s.x) or match it", e->name, e->name);
                emit(c, e->line, e->column, LOCAL_LOAD, c->aliases[alias].slot | 1u << 8 | (uint32_t)list << 16, 0);
                return item_record(c, e, list, c->aliases[alias].path, sub);
            }
            emit(c, e->line, e->column, LOCAL_LOAD, c->aliases[alias].slot | 1u << 8 | (uint32_t)list << 16, 0);
            int field = field_named(record_of(c, list), c->aliases[alias].path);
            emit(c, e->line, e->column, ITEM_GET, (uint32_t)list | (uint32_t)field << 16, 0);
            return field_value(record_of(c, list), (unsigned)field);
        }
        int param = parameter(c, e->name);
        if (param >= 0 && c->folding && c->scope == c->fold_scope && c->scope->spilled[param]) {
            int local = c->scope->spilled[param] - 1; /* the argument, put in a let before the fold */
            emit(c, e->line, e->column, LOCAL_LOAD, local_arg(c, local), 0);
            return c->locals[local].kind;
        }
        if (param >= 0 && c->folding && c->scope == c->fold_scope)
            fail_at(c, e->line, e->column,
                    "'%s' is a parameter outside this count, sum, max or min; its block reads only its item and states", e->name);
        if (param >= 0) {
            emit(c, e->line, e->column, PICK, (uint32_t)(height - 1 - (c->scope->base + param)), 0);
            return c->scope->kinds[param];
        }
        int fn = fn_index(c, c->module, e->name);
        if (fn >= 0 && c->fns[fn].called) return call_fn(c, e, fn, handler, height, NULL, 0);
        if (fn >= 0) return inline_fn(c, e, c->module, e->name, handler, height);
        if (c->module && c->current < 0)
            fail_at(c, e->line, e->column, "'%s' is not a parameter; a library fn reads only its parameters", e->name);
        int local = c->scope ? -1 : local_named(c, e->name);
        if (local >= 0 && c->locals[local].kind.type == RECORD) { /* a record's value held by name (027's 2c) */
            push_record(c, e->name, true, c->locals[local].kind.owner, e->line, e->column);
            return c->locals[local].kind;
        }
        if (local >= 0 && c->locals[local].variant && !e->raw)
            fail_at(c, e->line, e->column, "'%s' holds a variant: match it, as in match %s { ... }", e->name, e->name);
        if (local >= 0) { /* a let in the handler */
            if (c->locals[local].kind.type == BYTES || c->locals[local].kind.type == TEXT)
                emit(c, e->line, e->column, BYTES_LOAD, c->locals[local].slot, 0);
            else emit(c, e->line, e->column, LOCAL_LOAD, local_arg(c, local), 0);
            return c->locals[local].kind;
        }
        if (c->module) fail_at(c, e->line, e->column, "'%s' is not a parameter; a library fn reads only its parameters", e->name);
        if (!c->scope && c->arguments >= 0) { /* an argument of the view (a table's, in its columns and filter) */
            const Input *in = &c->inputs[c->arguments];
            for (unsigned f = 0; f < in->fields; ++f)
                if (!strcmp(in->field_names[f], e->name)) {
                    emit(c, e->line, e->column, FIELD, f, 0);
                    return (Kind){message_kind(in->field_types[f]), -1};
                }
        }
        if (!c->scope && c->row >= 0 && !strcmp(e->name, c->view_row[c->row])) {
            /* In a count, sum, max or min the column's row is still there (a
             * correlated fold: per branch, the sum of its accounts). */
            int list = state_index(c, c->view_list[c->row]);
            emit(c, e->line, e->column, c->folding ? OUTER_ROW : ROW, (uint32_t)list, 0);
            return (Kind){INDEX, list};
        }
        int index = state_index(c, e->name);
        if (index >= 0) {
            int type = c->state_types[index];
            if (type == STATE_LIST && c->state_event[index] && !c->state_hidden[index]) {
                /* a let's list as a value (a copy of its items): to a fn, a return, a let or a field */
                emit(c, e->line, e->column, LIST_VALUE, (uint32_t)index, 0);
                return (Kind){LIST, module_record(c, c->state_library[index], c->state_target[index])};
            }
            if (type == STATE_LIST)
                fail_at(c, e->line, e->column, "'%s' is a list; read %s.count or a field, as in %s[index].field",
                        e->name, e->name, e->name);
            static const uint8_t loads[] = {LOAD, TEXT_LOAD, CUR_LOAD, 0, INDEX_LOAD, LOAD, LOAD};
            static const Type types[] = {NUM, TEXT, CURSOR, 0, INDEX, BOOL, I64};
            emit(c, e->line, e->column, loads[type], (uint32_t)index, 0);
            if (type == STATE_TEXT && c->state_bytes[index]) return (Kind){BYTES, -1}; /* a bytes state's value (Pure 028) */
            return (Kind){types[type], type == STATE_CURSOR || type == STATE_INDEX ? cursor_owner(c, index) : -1};
        }
        int input = input_index(c, e->name);
        if (input >= 0 && c->inputs[input].kind == KIND_TEXT) {
            if (input != c->message) fail_at(c, e->line, e->column, "'%s' is only available in its handler", e->name);
            emit(c, e->line, e->column, FIELD, 0, 0); /* its one field */
            return (Kind){TEXT, -1};
        }
        const char *kind = declared_kind(c, e->name);
        if (kind) fail_at(c, e->line, e->column, "'%s' is %s, not a state", e->name, kind);
        fail_at(c, e->line, e->column, "unknown state '%s'", e->name);
    }
    case E_MEMBER: {
        const Expr *base = e->args[0];
        int item_list;
        char item[2 * NAME + 2];
        int timer = base->kind == E_NAME && !strcmp(e->member, "period") && !c->module && parameter(c, base->name) < 0 &&
                    local_named(c, base->name) < 0 ? input_index(c, base->name) : -1;
        if (timer >= 0 && c->inputs[timer].kind == KIND_TIMER) { /* tick.period: a timer's, in milliseconds, as now is */
            emit(c, e->line, e->column, CONST, 0, (double)c->inputs[timer].period);
            return (Kind){NUM, -1};
        }
        int binding = binding_path(c, e, item);
        if (binding >= 0) { /* down a record's value held by name (027's 2c) */
            Kind kind = binding_value(c, e, binding, item);
            if (kind.type) return kind;
        }
        int variant;
        unsigned which;
        const Expr *payload;
        if (variant_case(c, e, &variant, &which, &payload)) return made_record(c, e, variant, height); /* V.case */
        unsigned saved_code = c->code_count;
        if ((base->kind != E_NAME || alias_named(c, base->name) >= 0 || base->kind == E_ITEM) &&
            (base->kind == E_MEMBER || base->kind == E_ITEM || base->kind == E_INDEX || base->kind == E_NAME) &&
            item_path(c, e, handler, height, &item_list, item)) { /* item.at.x: a field down a path (027's 2b) */
            char *path = item;
            const Record *r = record_of(c, item_list);
            int sub = composite(r, path) ? sub_record(c, (int)(r - c->records), path) : -1;
            if (sub >= 0) return item_record(c, e, item_list, path, sub); /* a record's or a variant's value there (2c) */
            int field = field_named(r, path);
            size_t length = strlen(path);
            if (field < 0 && length > 5 && !strcmp(path + length - 5, ".case")) { /* a nested variant's case: its own field */
                path[length - 5] = 0;
                field = field_named(r, path);
                if (field < 0 || !r->field_variant[field]) path[length - 5] = '.', field = -1;
            }
            if (field >= 0) {
                if (r->field_variant[field] && !e->raw)
                    fail_at(c, e->line, e->column, "'%s' holds a variant: match it, as in match %s { ... }", path, path);
                emit(c, e->line, e->column, ITEM_GET, (uint32_t)item_list | (uint32_t)field << 16, 0);
                return field_value(r, (unsigned)field);
            }
            char *dot = strrchr(path, '.');
            if (dot) { /* item.items.count: a list in a field, then what a list has; item.name.bytes: a text's */
                *dot = 0;
                field = field_named(r, path);
                if (field >= 0 && r->field_kinds[field] == BYTES) { /* item.body.bytes: a bytes field's length */
                    emit(c, e->line, e->column, ITEM_GET, (uint32_t)item_list | (uint32_t)field << 16, 0);
                    if (strcmp(dot + 1, "bytes")) fail_at(c, e->line, e->column, "bytes have bytes, not '%s'", dot + 1);
                    emit(c, e->line, e->column, LEN, 0, 0);
                    return (Kind){NUM, -1};
                }
                if (field >= 0 && r->field_kinds[field] == TEXT) {
                    const char *m = dot + 1;
                    emit(c, e->line, e->column, ITEM_GET, (uint32_t)item_list | (uint32_t)field << 16, 0);
                    if (!strcmp(m, "chars") || !strcmp(m, "lines") || !strcmp(m, "bytes")) {
                        emit(c, e->line, e->column, m[0] == 'c' ? CHARS : m[0] == 'l' ? LINES : TEXT_BYTES, 0, 0);
                        return (Kind){NUM, -1};
                    }
                    fail_at(c, e->line, e->column, "text has chars, lines and bytes, not '%s'", m);
                }
                if (field >= 0 && r->field_kinds[field] == LIST) {
                    emit(c, e->line, e->column, ITEM_GET, (uint32_t)item_list | (uint32_t)field << 16, 0);
                    int reg = bind_list(c, e, field_value(r, (unsigned)field));
                    const char *m = dot + 1;
                    if (!strcmp(m, "count")) { emit(c, e->line, e->column, LIST_COUNT, (uint32_t)reg, 0); return (Kind){NUM, -1}; }
                    if (!strcmp(m, "first") || !strcmp(m, "last")) {
                        emit(c, e->line, e->column, m[0] == 'f' ? INDEX_FIRST : INDEX_LAST, (uint32_t)reg, 0);
                        return item_value(c, e, reg);
                    }
                    fail_at(c, e->line, e->column, "a list has count, first and last, not '%s'", m);
                }
                *dot = '.';
            }
            if (holds_under(r, path))
                fail_at(c, e->line, e->column, "'%s' holds a record or a variant: read its fields (%s.x) or match it", path, path);
            fail_at(c, e->line, e->column, "record '%s' has no field '%s'", record_shown(c, c->module, r), path);
        }
        c->code_count = saved_code;
        int library = base->kind == E_NAME && parameter(c, base->name) < 0 ? used_library(c, c->module, base->name) : -1;
        if (library >= 0) return inline_fn(c, e, library, e->member, handler, height); /* library.constant */
        int message;
        char path[NAME];
        int given = rows_list(c, e);
        if (given >= 0) { /* a message's rows as a value (a copy of them): to a fn, a return, a let or a field */
            emit(c, e->line, e->column, LIST_VALUE, (uint32_t)given, 0);
            return (Kind){LIST, module_record(c, c->state_library[given], c->state_target[given])};
        }
        if (message_path(c, e, &message, path)) {
            const Input *in = &c->inputs[message];
            if (message != c->message)
                fail_at(c, e->line, e->column, "fields of '%s' are only available in its handler", in->name);
            size_t length = strlen(path);
            for (unsigned i = 0; i < in->fields; ++i)
                if (!strcmp(in->field_names[i], path)) {
                    emit(c, e->line, e->column, FIELD, i, 0);
                    return (Kind){message_kind(in->field_types[i]), -1};
                }
            for (unsigned i = 0; i < in->fields; ++i)
                if (!strncmp(in->field_names[i], path, length) && in->field_names[i][length] == '.')
                    fail_at(c, e->line, e->column, "'%s.%s' is a group of fields; read one of them, as in %s.%s",
                            in->name, path, in->name, in->field_names[i]);
            fail_at(c, e->line, e->column, "message '%s' has no field '%s'", in->name, path);
        }
        if (base->kind == E_NAME && parameter(c, base->name) < 0 && !c->module) {
            int input = input_index(c, base->name);
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
                    return item_value(c, e, state);
                }
                fail_at(c, e->line, e->column, "a list has count, first and last, not '%s'", m);
            }
        }
        int rows = rows_list(c, base);
        if (rows >= 0) { /* order.lines.count: a message's rows are a list */
            const char *m = e->member;
            if (!strcmp(m, "count")) { emit(c, e->line, e->column, LIST_COUNT, (uint32_t)rows, 0); return (Kind){NUM, -1}; }
            if (!strcmp(m, "first") || !strcmp(m, "last")) {
                emit(c, e->line, e->column, m[0] == 'f' ? INDEX_FIRST : INDEX_LAST, (uint32_t)rows, 0);
                return (Kind){INDEX, rows};
            }
            fail_at(c, e->line, e->column, "a message's rows have count, first and last, not '%s'", m);
        }
        Kind inner = value(c, base, handler, height);
        const char *m = e->member;
        if (inner.type == TEXT && (!strcmp(m, "chars") || !strcmp(m, "lines") || !strcmp(m, "bytes"))) {
            emit(c, e->line, e->column, m[0] == 'c' ? CHARS : m[0] == 'l' ? LINES : TEXT_BYTES, 0, 0);
            return (Kind){NUM, -1};
        }
        if (inner.type == BYTES && !strcmp(m, "bytes")) { emit(c, e->line, e->column, LEN, 0, 0); return (Kind){NUM, -1}; }
        if (inner.type == CURSOR && (!strcmp(m, "chars") || !strcmp(m, "line"))) {
            emit(c, e->line, e->column, CUR_COUNT, IN(inner.owner, m[0] == 'c' ? UNIT_CHAR : UNIT_LINE), 0);
            if (m[0] == 'l') { /* at.line counts from 1, Pure's lines from 0 */
                emit(c, e->line, e->column, CONST, 0, 1);
                emit(c, e->line, e->column, ADD, 0, 0);
            }
            return (Kind){NUM, -1};
        }
        if (inner.type == INDEX && !strcmp(m, "position")) {
            emit(c, e->line, e->column, INDEX_NUM, (uint32_t)inner.owner, 0);
            return (Kind){NUM, -1};
        }
        if (inner.type == LIST) { /* a list value: bound to read */
            int reg = bind_list(c, e, inner);
            if (!strcmp(m, "count")) { emit(c, e->line, e->column, LIST_COUNT, (uint32_t)reg, 0); return (Kind){NUM, -1}; }
            if (!strcmp(m, "first") || !strcmp(m, "last")) {
                emit(c, e->line, e->column, m[0] == 'f' ? INDEX_FIRST : INDEX_LAST, (uint32_t)reg, 0);
                return item_value(c, e, reg);
            }
            fail_at(c, e->line, e->column, "a list has count, first and last, not '%s'", m);
        }
        if (inner.type == INDEX) {
            /* at.title: a field of the item the index names; a record's or a variant's value there (027's 2c). */
            const Record *r = record_of(c, inner.owner);
            int sub = composite(r, m) ? sub_record(c, (int)(r - c->records), m) : -1;
            if (sub >= 0) return item_record(c, e, inner.owner, m, sub);
            if (field_named(r, m) < 0 && holds_under(r, m))
                fail_at(c, e->line, e->column, "'%s' holds a record or a variant: read its fields (%s.x) or match it", m, m);
            unsigned field = record_field(c, r, m, e->line, e->column);
            if (r->field_variant[field] && !e->raw)
                fail_at(c, e->line, e->column, "'%s' holds a variant: match it, as in match %s { ... }", m, m);
            emit(c, e->line, e->column, ITEM_GET, (uint32_t)inner.owner | field << 16, 0);
            return field_value(r, field);
        }
        fail_at(c, e->line, e->column, "%s has no '%s'", type_name(inner.type), m);
    }
    case E_FN: {
        int fn = fn_index(c, c->module, e->name);
        if (!*e->member && fn >= 0 && c->fns[fn].called) return call_fn(c, e, fn, handler, height, NULL, 0);
        if (!*e->member) return inline_fn(c, e, c->module, e->name, handler, height);
        int library = parameter(c, e->member) < 0 ? used_library(c, c->module, e->member) : -1;
        fn = library >= 0 ? fn_index(c, library, e->name) : -1;
        if (fn >= 0 && c->fns[fn].called) return call_fn(c, e, fn, handler, height, NULL, 0);
        if (library >= 0) return inline_fn(c, e, library, e->name, handler, height);
        int variant;
        unsigned which;
        const Expr *payload;
        if (variant_case(c, e, &variant, &which, &payload)) return made_record(c, e, variant, height); /* V.case(x) */
        int list = state_index(c, e->member);
        if (!strcmp(e->name, "has") && list >= 0 && c->state_types[list] == STATE_LIST) {
            /* people.has("bob"): whether a keyed list holds that key. */
            app_name(c, e, e->member);
            const char *key = keyed_by(c, list);
            if (!key) fail_at(c, e->line, e->column, "'%s' has no key; declare it as list of %s by a field to ask has",
                              e->member, record_shown(c, c->module, record_of(c, list)));
            if (e->count != 1) fail_at(c, e->line, e->column, "%s.has takes one key", e->member);
            Type type = field_kind(record_of(c, list), (unsigned)c->list_key[list] - 1);
            Kind given = wanted(c, e->args[0], handler, height, type);
            if (given.type != type)
                fail_at(c, e->args[0]->line, e->args[0]->column, "'%s' is keyed by %s, %s: expected %s, not %s", e->member, key,
                        type_name(type), type_name(type), type_name(given.type));
            emit(c, e->line, e->column, LIST_HAS, (uint32_t)list, 0);
            return (Kind){BOOL, -1};
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
    case E_CONVERT: {
        const Expr *a = e->args[0];
        int list = e->op == BYTES_FROM && a->kind == E_NAME && !c->module && parameter(c, a->name) < 0 ? state_index(c, a->name) : -1;
        if (list >= 0 && c->state_types[list] == STATE_LIST) { /* bytes(xs): a list of i64s, each a byte */
            list = list_state(c, a->name, a->line, a->column, NULL);
            const Record *r = record_of(c, list);
            if (!r->scalar || r->field_kinds[0] != I64)
                fail_at(c, a->line, a->column, "bytes(...) makes bytes of a list of i64, not of %s", record_shown(c, c->module, r));
            emit(c, e->line, e->column, BYTES_OF, (uint32_t)list, 0);
            return (Kind){BYTES, -1};
        }
        Kind inner = value(c, e->args[0], handler, height);
        Type from = e->op == TEXT_FROM ? BYTES : TEXT;
        if (inner.type != from)
            fail_at(c, e->line, e->column, "%s(...) converts %s, not %s", e->op == TEXT_FROM ? "text" : "bytes",
                    type_name(from), type_name(inner.type));
        emit(c, e->line, e->column, (uint8_t)e->op, 0, 0);
        return (Kind){e->op == TEXT_FROM ? TEXT : BYTES, -1};
    }
    case E_ITEM: {
        int named = c->module || parameter(c, e->name) >= 0 ? -1 : state_index(c, e->name);
        if (!*e->member && (named < 0 || c->state_types[named] != STATE_LIST)) {
            Expr name = *e, at = *e; /* data[i]: a parameter's, an argument's, a let's or a state's byte; data[a .. c] */
            name.kind = E_NAME;
            name.count = 0;
            at.kind = E_INDEX;
            at.args[0] = &name;
            at.args[1] = e->args[0];
            at.args[2] = e->count == 2 ? e->args[1] : NULL;
            at.count = e->count + 1;
            return value(c, &at, handler, height);
        }
        if (e->count == 2) fail_at(c, e->line, e->column, "a list's slice goes in a for or a fold, as in for x in %s[a .. b]", e->name);
        int local = c->module ? -1 : local_named(c, e->name), list;
        if (!c->module && (parameter(c, e->name) >= 0 || (local >= 0 && c->locals[local].kind.type == LIST)))
            list = walked_list_at(c, e->name, e->line, e->column, height); /* a list value, bound to be read */
        else {
            app_name(c, e, e->name);
            list = list_state(c, e->name, e->line, e->column, e->args[0]);
        }
        const Record *r = record_of(c, list);
        if (!*e->member && r->scalar) { /* xs[i]: the value at a position (or a key) */
            item_of(c, e->args[0], handler, height, list);
            emit(c, e->line, e->column, ITEM_GET, (uint32_t)list, 0);
            return field_value(r, 0);
        }
        if (!*e->member && !keyed_by(c, list)) { /* xs[i]: the item at a position */
            item_of(c, e->args[0], handler, height, list);
            return (Kind){INDEX, list};
        }
        if (!*e->member) {
            /* people["bob"]: the index of the item with that key. */
            item_of(c, e->args[0], handler, height, list);
            if (c->code[c->code_count - 1].op != LIST_FIND)
                fail_at(c, e->line, e->column, "%s[index] is the index itself; read a field, as in %s[index].%s", e->name,
                        e->name, r->field_names[0]);
            return (Kind){INDEX, list};
        }
        unsigned field = record_field(c, r, e->member, e->line, e->column);
        item_of(c, e->args[0], handler, height, list);
        emit(c, e->line, e->column, ITEM_GET, (uint32_t)list | field << 16, 0);
        return field_value(r, field);
    }
    case E_INDEX: {
        int given = e->count == 2 ? rows_list(c, e->args[0]) : -1;
        if (given >= 0) { /* a message's rows' item at a position: read where they are, not from a copy of them */
            item_of(c, e->args[1], handler, height, given);
            return item_value(c, e, given);
        }
        Kind base = value(c, e->args[0], handler, height);
        if (base.type == LIST && e->count == 2) { /* a list value's item at a position */
            int reg = bind_list(c, e, base);
            item_of(c, e->args[1], handler, height, reg);
            return item_value(c, e, reg);
        }
        if (e->count == 3) { /* b[a .. c]: bytes, or a text's (whose cut must fall between characters) */
            if (base.type != BYTES && base.type != TEXT)
                fail_at(c, e->line, e->column, "only bytes and texts are sliced, not %s", type_name(base.type));
            if (base.type == TEXT) emit(c, e->line, e->column, BYTES_FROM, 0, 0);
            Kind low = value(c, e->args[1], handler, height + 1);
            if (low.type != NUM && low.type != I64)
                fail_at(c, e->args[1]->line, e->args[1]->column, "a position is a number or an i64, not %s", type_name(low.type));
            Kind high = wanted(c, e->args[2], handler, height + 2, low.type);
            if (high.type != low.type)
                fail_at(c, e->args[2]->line, e->args[2]->column, "a slice's ends are both numbers or both i64s");
            emit(c, e->line, e->column, BYTES_SLICE, low.type == I64, 0);
            if (base.type == TEXT) emit(c, e->line, e->column, TEXT_FROM, 0, 0);
            return (Kind){base.type, -1};
        }
        if (base.type == TEXT)
            fail_at(c, e->line, e->column, "a text is read by position through its bytes, as in bytes(t)[i]");
        if (base.type != BYTES) fail_at(c, e->line, e->column, "only bytes are read by position, not %s", type_name(base.type));
        Kind at = value(c, e->args[1], handler, height + 1);
        if (at.type != NUM && at.type != I64)
            fail_at(c, e->args[1]->line, e->args[1]->column, "a position is a number or an i64, not %s", type_name(at.type));
        emit(c, e->line, e->column, BYTE_AT, at.type == I64, 0);
        return (Kind){I64, -1};
    }
    case E_LIST:
        fail_at(c, e->line, e->column, "a list's elements begin a let's list, as in let xs: list of i64 = [1, 2]");
    case E_IF: {
        /* Arms of an else-if chain share one end; a value crosses it, so the
         * branches ahead that carry one are counted against Pure's limit. An
         * integer literal branch takes the other branches' type. */
        unsigned ends[DEPTH], arms = 0, saved = c->joins;
        Kind kind = {0, -1};
        /* Literal branches take the type of the first that is no literal (an i64's, then). */
        Type hint = want == I64 ? I64 : 0;
        for (const Expr *at = e; !hint; at = at->args[2]) {
            if (!literal_only(at->args[1])) hint = probe(c, at->args[1], handler, height) == I64 ? I64 : NUM;
            else if (at->args[2]->kind != E_IF) hint = literal_only(at->args[2]) ? NUM : probe(c, at->args[2], handler, height) == I64 ? I64 : NUM;
            else continue;
        }
        const Expr *arm = e;
        for (;;) {
            truth(c, arm->args[0], handler, height, "if");
            unsigned jump = c->code_count;
            emit(c, arm->line, arm->column, JZ, 0, 0);
            if (height && ++c->joins > JOINS) fail_at(c, arm->line, arm->column, "conditional expressions nest too deeply");
            if_branch(c, arm->args[1], handler, height, hint, &kind);
            ends[arms++] = c->code_count;
            emit(c, arm->line, arm->column, JMP, 0, 0);
            if (arms == 1 && ++c->joins > JOINS) fail_at(c, arm->line, arm->column, "conditional expressions nest too deeply");
            c->code[jump].arg = c->code_count;
            if (height) --c->joins;
            if (arm->args[2]->kind != E_IF) break;
            arm = arm->args[2];
        }
        if_branch(c, arm->args[2], handler, height, hint, &kind);
        for (unsigned i = 0; i < arms; ++i) c->code[ends[i]].arg = c->code_count;
        c->joins = saved;
        return kind;
    }
    case E_FOLD: {
        /* The block after FOLD runs once per item, as a column of the list: it
         * starts with an empty stack and reads only its item and states. */
        int list = walked_list_at(c, e->member, e->line, e->column, height); /* rows in its handler, a list value bound */
        if (c->folding) fail_at(c, e->line, e->column, "a count, sum, max or min cannot hold another");
        if (c->filtering) fail_at(c, e->line, e->column, "a table's where cannot hold a count, sum, max or min");
        bool sliced = e->args[2] != NULL;
        if (sliced) { /* the keys from low up to before high, found by key */
            Type key = keyed_by(c, list) ? field_kind(record_of(c, list), (unsigned)c->list_key[list] - 1) : TEXT;
            if (key != NUM && key != I64)
                fail_at(c, e->line, e->column, "'%s' is not keyed by a number or an i64: a slice takes one", e->member);
            expect_kind(c, e->args[2], wanted(c, e->args[2], handler, height, key), key);
            expect_kind(c, e->args[3], wanted(c, e->args[3], handler, height + 1, key), key);
        }
        if (list > 0xFF) /* a fold names its list in a byte of its argument (Pure's FOLD) */
            fail_at(c, e->line, e->column, "a folded list is among the first 256 states; '%s' is state %d", e->member, list);
        /* In an inlined fn, the arguments its block reads go into lets first: the block sees no stack. */
        bool spilled = false;
        if (c->scope) {
            bool reads[PARAMS] = {false};
            for (unsigned i = 0; i < 2; ++i) reads_parameters(c, e->args[i], e->name, reads);
            const Fn *f = &c->fns[c->scope->fn];
            for (unsigned p = 0; p < f->param_count; ++p) {
                Kind kind = c->scope->kinds[p];
                if (!reads[p] || (kind.type != NUM && kind.type != BOOL && kind.type != I64 && kind.type != INDEX)) continue;
                char hidden[NAME];
                snprintf(hidden, sizeof(hidden), "$%u$%.24s", p, f->params[p]); /* no name the source can write */
                name_value(c, hidden, kind, e->line, e->column);
                int local = (int)c->local_count - 1;
                emit(c, e->line, e->column, PICK, (uint32_t)(height - 1 - (c->scope->base + (int)p)), 0);
                emit(c, e->line, e->column, LOCAL_STORE, c->locals[local].slot, 0);
                c->scope->spilled[p] = local + 1;
                spilled = true;
            }
        }
        unsigned at = c->code_count, joins = c->joins;
        emit(c, e->line, e->column, FOLD, 0, 0);
        c->folding = true;
        c->fold_list = list;
        c->fold_scope = c->scope;
        memcpy(c->fold_name, e->name, NAME);
        c->joins = 0;
        bool itemwise = e->op == FOLD_COUNT || e->op == FOLD_FIRST;
        Type values = NUM;
        if (!itemwise) {
            Kind kind = value(c, e->args[1], false, 0);
            if (kind.type != NUM && kind.type != I64)
                fail_at(c, e->args[1]->line, e->args[1]->column, "sum, max, min and distinct take numbers or i64s, not %s",
                        type_name(kind.type));
            values = kind.type;
        }
        if (e->args[0]) truth(c, e->args[0], false, !itemwise, "where");
        else emit(c, e->line, e->column, CONST, 1, 1); /* every item */
        c->folding = false;
        c->fold_list = -1;
        c->joins = joins;
        if (spilled) memset(c->scope->spilled, 0, sizeof(c->scope->spilled));
        c->code[at].arg = (uint32_t)e->op | (values == I64 ? FOLD_INTEGER : 0) | (sliced ? 0x80u : 0) | (uint32_t)list << 8 |
                          (uint32_t)(c->code_count - at - 1) << 16;
        return e->op == FOLD_FIRST ? (Kind){INDEX, list} :
               (Kind){values == I64 && e->op != FOLD_DISTINCT ? I64 : NUM, -1}; /* an i64's sum, most or least */
    }
    case E_RECORD: { /* R { ... }: its fields onto the stack (027's 2c) */
        int from = e->member[0] ? used_library(c, c->module, e->member) : c->module;
        int record = from >= 0 ? module_record(c, from, e->name) : -1;
        if (record < 0) fail_at(c, e->line, e->column, "unknown record '%s'", e->name);
        return made_record(c, e, record, height);
    }
    case E_FIELD:
        fail_at(c, e->line, e->column, "a record value goes into a list, as in list.append(%s { ... })", e->name);
    case E_UNIT:
        fail_at(c, e->line, e->column, "a distance moves a cursor, as in at + 1 char");
    case E_BINARY:
        if (e->op == ADD || e->op == SUB) {
            /* at + 1 char moves a cursor, at + 1 an index; both stop at the ends. */
            bool distance = e->args[1]->kind == E_UNIT;
            Kind left = !distance && meets_bytes(c, e->args[0], e->args[1], handler, height + 1)
                            ? meeting(c, e->args[0], handler, height, BYTES)
                            : wanted(c, e->args[0], handler, height, distance ? 0 : left_hint(c, e, handler, height, want));
            if (!distance && (left.type == BYTES || left.type == TEXT)) { /* joined */
                if (e->op == SUB) fail_at(c, e->line, e->column, "texts and bytes are joined with +; - takes nothing apart");
                Kind right = meeting(c, e->args[1], handler, height + 1, left.type);
                if (right.type != left.type)
                    fail_at(c, e->args[1]->line, e->args[1]->column, "%s joins %s, not %s", type_name(left.type), type_name(left.type),
                            type_name(right.type));
                emit(c, e->line, e->column, CONCAT, left.type == BYTES, 0);
                return left;
            }
            if (distance && left.type != CURSOR)
                fail_at(c, e->line, e->column, "only a cursor moves by a distance, not %s", type_name(left.type));
            if (distance || left.type == INDEX) {
                number(c, distance ? e->args[1]->args[0] : e->args[1], handler, height + 1);
                if (e->op == SUB) emit(c, e->line, e->column, NEG, 0, 0);
                if (distance) emit(c, e->line, e->column, CUR_MOVE, IN(left.owner, e->args[1]->op), 0);
                else emit(c, e->line, e->column, INDEX_MOVE, (uint32_t)left.owner, 0);
                return left;
            }
            Type type = second(c, e, left, handler, height);
            emit(c, e->line, e->column, type == I64 ? i64_op(e->op) : (uint8_t)e->op, 0, 0);
            return (Kind){type, -1};
        }
        if (e->op >= LT && e->op <= NE) {
            /* Texts and bytes compare by their bytes, numbers and i64s as numbers; bools are equal or not. */
            Kind left = meets_bytes(c, e->args[0], e->args[1], handler, height + 1) ? meeting(c, e->args[0], handler, height, BYTES)
                                                                                    : wanted(c, e->args[0], handler, height, left_hint(c, e, handler, height, 0));
            if (left.type == TEXT || left.type == BYTES) {
                Kind right = meeting(c, e->args[1], handler, height + 1, left.type);
                if (right.type == TEXT || right.type == BYTES) {
                    if (right.type != left.type)
                        fail_at(c, e->args[1]->line, e->args[1]->column, "%s compares with %s, not %s: convert one, as in bytes(t) or "
                                "text(b)", type_name(left.type), type_name(left.type), type_name(right.type));
                } else expect_kind(c, e->args[1], right, left.type);
                emit(c, e->line, e->column, TEXT_COMPARE, (uint32_t)(e->op - LT) | (left.type == BYTES ? 0x100u : 0), 0);
                return (Kind){BOOL, -1};
            }
            if (left.type == BOOL) {
                if (e->op != EQ && e->op != NE) fail_at(c, e->line, e->column, "bools are equal or not (== and !=); they have no order");
                expect_kind(c, e->args[1], value(c, e->args[1], handler, height + 1), BOOL);
                emit(c, e->line, e->column, (uint8_t)e->op, 0, 0);
                return (Kind){BOOL, -1};
            }
            if (second(c, e, left, handler, height) == I64) emit(c, e->line, e->column, I64_COMPARE, (uint32_t)(e->op - LT), 0);
            else emit(c, e->line, e->column, (uint8_t)e->op, 0, 0);
            return (Kind){BOOL, -1};
        }
        /* fall through */
    default: {
        if (e->op >= BIT_AND && e->op <= BIT_NOT) { /* &, |, ^, <<, >> and ~: on i64s, as integers without end (Pure 027) */
            for (unsigned i = 0; i < e->count; ++i) {
                bool count = i == 1 && (e->op == BIT_LEFT || e->op == BIT_RIGHT); /* a number or an i64, as a position is */
                Kind kind = count && !loose(e->args[i]) ? value(c, e->args[i], handler, height + 1)
                                                        : wanted(c, e->args[i], handler, height + (int)i, I64);
                if (count && kind.type == NUM) {
                    emit(c, e->args[i]->line, e->args[i]->column, I64_FROM, 0, 0);
                    continue;
                }
                if (kind.type != I64)
                    fail_at(c, e->args[i]->line, e->args[i]->column, kind.type == NUM ?
                            "bit operations take i64s, not numbers: convert, as in i64(x)" : "bit operations take i64s, not %s",
                            type_name(kind.type));
            }
            if (e->op == BIT_NOT) emit_integer(c, e->line, e->column, -1);
            static const uint8_t how[] = {0, 1, 2, 3, 4, 2}; /* Pure's PureBits: and, or, xor, left, right; ~ is xor -1 */
            emit(c, e->line, e->column, I64_BITS, how[e->op - BIT_AND], 0);
            return (Kind){I64, -1};
        }
        /* !, && and || take bools (both sides are computed); *, /, %, min and
         * max numbers or i64s; clamp and floor numbers. */
        bool logic = e->op == AND || e->op == OR || e->op == NOT;
        const Expr *outer = c->given;
        c->given = e->args[0];
        bool eager = (e->op != AND && e->op != OR) || faultless(c, e->args[1]);
        c->given = outer;
        if (!eager) {
            /* a && b is false and a || b true without b once a says so: b, which could fail, then runs only where it
             * decides (a branch; where b cannot fail, AND and OR give the same and compute both) */
            truth(c, e->args[0], handler, height, e->op == AND ? "&&" : "||");
            unsigned skip = c->code_count;
            emit(c, e->line, e->column, JZ, 0, 0);
            if (height && ++c->joins > JOINS) fail_at(c, e->line, e->column, "conditional expressions nest too deeply");
            if (e->op == AND) truth(c, e->args[1], handler, height, "&&");
            else emit(c, e->line, e->column, CONST, 1, 1);
            unsigned end = c->code_count;
            emit(c, e->line, e->column, JMP, 0, 0);
            if (++c->joins > JOINS) fail_at(c, e->line, e->column, "conditional expressions nest too deeply");
            c->code[skip].arg = c->code_count;
            if (e->op == AND) emit(c, e->line, e->column, CONST, 1, 0);
            else truth(c, e->args[1], handler, height, "||");
            c->code[end].arg = c->code_count;
            c->joins -= 1 + (height > 0);
            return (Kind){BOOL, -1};
        }
        if (logic) {
            for (unsigned i = 0; i < e->count; ++i)
                truth(c, e->args[i], handler, height + (int)i, *e->member ? e->member : e->op == NOT ? "!" : e->op == AND ? "&&" : "||");
            emit(c, e->line, e->column, (uint8_t)e->op, 0, 0);
            return (Kind){BOOL, -1};
        }
        if (e->op == ABS) { /* abs(x): x once, then the greater of it and its negation (-0's is +0, as IEEE's maximum) */
            Kind operand = wanted(c, e->args[0], handler, height, want);
            if (operand.type != I64) expect_kind(c, e->args[0], operand, NUM);
            if (height + 2 > STACK) fail_at(c, e->line, e->column, "expression needs more than 64 stack values");
            emit(c, e->line, e->column, PICK, 0, 0);
            emit(c, e->line, e->column, operand.type == I64 ? I64_NEG : NEG, 0, 0);
            emit(c, e->line, e->column, operand.type == I64 ? I64_MAX : MAX, 0, 0);
            return (Kind){operand.type, -1};
        }
        if (e->op == BYTE) { /* byte(n): one byte, n an i64 0-255 */
            integer(c, e->args[0], handler, height);
            emit(c, e->line, e->column, BYTE, 0, 0);
            return (Kind){BYTES, -1};
        }
        if (e->op == CHAR) { /* char(n): the text of a code point, n an i64 (027's 2c) */
            integer(c, e->args[0], handler, height);
            emit(c, e->line, e->column, CHAR, 0, 0);
            return (Kind){TEXT, -1};
        }
        if (e->op == I64_FROM) { /* i64(x): a whole number in range; a literal is one exactly */
            if (loose(e->args[0])) integer(c, e->args[0], handler, height);
            else {
                number(c, e->args[0], handler, height);
                emit(c, e->line, e->column, I64_FROM, 0, 0);
            }
            return (Kind){I64, -1};
        }
        if (e->op == NUM_FROM) { /* number(i): exact within 2^53; number(t): a text written as a JSON number */
            if (loose(e->args[0])) return value(c, e->args[0], handler, height);
            if (probe(c, e->args[0], handler, height) == TEXT) {
                value(c, e->args[0], handler, height);
                emit(c, e->line, e->column, NUM_PARSE, 0, 0);
                return (Kind){NUM, -1};
            }
            integer(c, e->args[0], handler, height);
            emit(c, e->line, e->column, NUM_FROM, 0, 0);
            return (Kind){NUM, -1};
        }
        if (e->op == NEG) {
            Kind operand = wanted(c, e->args[0], handler, height, want);
            if (operand.type == I64) {
                emit(c, e->line, e->column, I64_NEG, 0, 0);
                return operand;
            }
            expect_kind(c, e->args[0], operand, NUM);
            emit(c, e->line, e->column, NEG, 0, 0);
            return (Kind){NUM, -1};
        }
        if (e->count == 2 && (e->op == MUL || e->op == DIV || e->op == FLOORDIV || e->op == MOD || e->op == MIN || e->op == MAX)) {
            Kind left = wanted(c, e->args[0], handler, height, left_hint(c, e, handler, height, want));
            Type type = second(c, e, left, handler, height);
            if (type == I64 && e->op == DIV) /* Pure 027's stage 5: / divides numbers, // gives an i64's floored quotient */
                fail_at(c, e->line, e->column, "an i64 divides by // (its quotient, floored); / divides numbers, as in "
                        "number(a) / number(b)");
            emit(c, e->line, e->column, type == I64 ? i64_op(e->op) : (uint8_t)e->op, 0, 0);
            return (Kind){type, -1};
        }
        for (unsigned i = 0; i < e->count; ++i) number(c, e->args[i], handler, height + (int)i);
        emit(c, e->line, e->column, (uint8_t)e->op, 0, 0);
        return (Kind){NUM, -1};
    }
    }
}

/* Bytes, or text sent as its UTF-8 bytes. */
static void bytes_value(Compiler *c, const Expr *e, int height, const char *what) {
    Kind kind = value(c, e, c->handling, height);
    if (kind.type == TEXT) emit(c, e->line, e->column, BYTES_FROM, 0, 0);
    else if (kind.type != BYTES) fail_at(c, e->line, e->column, "%s takes bytes or text, not %s", what, type_name(kind.type));
}

/* The spread among a record's or a send's fields (..source), or NULL. */
static const Expr *spread_of(const Expr *e) {
    for (const Expr *f = e->args[0]; f; f = f->next)
        if (!f->member[0]) return f;
    return NULL;
}

/* What a spread's source reads name from: source.name, as written out (an
 * item's field, or a field of the handler's message by its path), or NULL
 * when the source has no field of that name. A source is an item (an
 * index, a let or the item a for walks, or list[key]) or the handler's
 * message. */
static Expr *spread_field(Compiler *c, const Expr *spread, const char *name) {
    const Expr *source = spread->args[0];
    int line = spread->line, column = spread->column, list = -1;
    if (source->kind == E_NAME && c->message >= 0 && !strcmp(source->name, c->inputs[c->message].name)) {
        const Input *in = &c->inputs[c->message];
        bool has = false;
        for (unsigned i = 0; i < in->fields && !has; ++i) has = !strcmp(in->field_names[i], name);
        if (!has) return NULL;
        Expr *read = node(c, E_NAME, line, column);
        memcpy(read->name, source->name, NAME);
        for (const char *part = name; *part;) { /* to.city: message.to.city */
            size_t length = strcspn(part, ".");
            Expr *access = node(c, E_MEMBER, line, column);
            access->args[0] = read;
            access->count = 1;
            memcpy(access->member, part, length);
            read = access;
            part += length + (part[length] == '.');
        }
        return read;
    }
    if (source->kind == E_NAME) {
        int local = local_named(c, source->name), state = state_index(c, source->name);
        int loop = loop_named(c, source->name);
        if (loop >= 0 && c->loops[loop].list >= 0) list = c->loops[loop].list;
        else if (local >= 0 && c->locals[local].kind.type == INDEX) list = c->locals[local].kind.owner;
        else if (local < 0 && state >= 0 && c->state_types[state] == STATE_INDEX) list = cursor_owner(c, state);
    } else if (source->kind == E_ITEM && !source->member[0])
        list = list_state(c, source->name, source->line, source->column, source->args[0]);
    if (list < 0)
        fail_at(c, source->line, source->column, "a spread takes an item (an index, or list[key]) or this handler's message");
    const Record *r = record_of(c, list);
    bool has = false;
    for (unsigned i = 0; i < r->fields && !has; ++i) has = !strcmp(r->field_names[i], name);
    if (!has) return NULL;
    Expr *read;
    if (source->kind == E_ITEM) {
        read = node(c, E_ITEM, line, column);
        memcpy(read->name, source->name, NAME);
        read->args[0] = source->args[0];
        read->count = 1;
    } else {
        Expr *item = node(c, E_NAME, line, column);
        memcpy(item->name, source->name, NAME);
        read = node(c, E_MEMBER, line, column);
        read->args[0] = item;
        read->count = 1;
    }
    memcpy(read->member, name, NAME);
    return read;
}

/* A record value's fields in the record's order (values, NULL when not
 * given): those written, and the rest its spread's source has by name. */
/* A variant's case a constructor names (027's 2b): Variant.case(value) or Variant.case; false for anything else. */
static bool variant_case(Compiler *c, const Expr *e, int *variant, unsigned *index, const Expr **payload) {
    const char *type, *label;
    *payload = NULL;
    if (e->kind == E_FN && *e->member) {
        type = e->member, label = e->name;
        *payload = e->count ? e->args[0] : NULL;
    } else if (e->kind == E_MEMBER && e->args[0]->kind == E_NAME) {
        type = e->args[0]->name, label = e->member;
    } else return false;
    int found = c->module ? -1 : module_record(c, 0, type);
    if (found < 0 || !c->records[found].variant) return false;
    if (e->kind == E_FN && e->count > 1) fail_at(c, e->line, e->column, "%s.%s takes one value", type, label);
    const Record *v = &c->records[found];
    for (unsigned i = 0; i < v->declared_count; ++i) {
        if (strcmp(v->declared[i].name, label)) continue;
        bool valued = v->declared[i].kind || *v->declared[i].type;
        if (valued && !*payload) fail_at(c, e->line, e->column, "%s.%s holds a value: give it, as in %s.%s(...)", type, label, type, label);
        if (!valued && *payload) fail_at(c, e->line, e->column, "%s.%s holds no value", type, label);
        *variant = found, *index = i;
        return true;
    }
    fail_at(c, e->line, e->column, "variant '%s' has no case '%s'", type, label);
}

/* Whether r holds a record's or a variant's fields at path (at.x, or a variant's case named path). */
static bool composite(const Record *r, const char *path) {
    int field = field_named(r, path);
    if (field >= 0 && r->field_variant[field]) return true;
    size_t n = strlen(path);
    for (unsigned i = 0; i < r->fields; ++i) if (!strncmp(r->field_names[i], path, n) && r->field_names[i][n] == '.') return true;
    return false;
}

/* A literal of an i64 (a variant's case number). */
static Expr *case_number(Compiler *c, const Expr *at, unsigned index) {
    Expr *n = node(c, E_NUMBER, at->line, at->column);
    n->number = index, n->integer = index, n->whole = n->hex = true;
    return n;
}

/* x's field at a path (an item's, a match's binding's), as an expression. */
static Expr *member_of(Compiler *c, const Expr *x, const char *path) {
    Expr *m = node(c, E_MEMBER, x->line, x->column);
    m->args[0] = (Expr *)x;
    m->count = 1;
    m->raw = true;
    snprintf(m->member, NAME, "%s", path);
    return m;
}

/* The values of r's fields under prefix ("": all of them; a variant's case is the field named prefix, or case) from x:
 * a record written out, a variant's case, or an item (an index, a match's binding) whose fields of those names give
 * them (027's 2b). label names where it goes, for messages. */
static const Expr *settled(Compiler *c, const Expr *x, int height);
static void values_under(Compiler *c, const Record *r, const char *prefix, const Expr *x, const Expr **values, const char *label,
                         int height) {
    char at[2 * NAME + 2];
    x = settled(c, x, height);
    int tag = field_named(r, *prefix ? prefix : "case");
    int variant = tag >= 0 ? r->field_variant[tag] - 1 : -1;
    int which;
    unsigned index;
    const Expr *payload;
    if (variant_case(c, x, &which, &index, &payload)) {
        if (variant < 0 || which != variant)
            fail_at(c, x->line, x->column, "%s holds %s, not %s", *prefix ? prefix : label,
                    variant >= 0 ? c->records[variant].name : "no variant", c->records[which].name);
        values[tag] = case_number(c, x, index);
        if (!payload) return;
        const char *name = c->records[which].declared[index].name;
        snprintf(at, sizeof(at), "%s%s%s", prefix, *prefix ? "." : "", name);
        if (composite(r, at)) values_under(c, r, at, payload, values, label, height); /* a record's or a variant's fields */
        else values[field_named(r, at)] = payload;
        return;
    }
    if (x->kind == E_RECORD) {
        for (const Expr *f = x->args[0]; f; f = f->next) {
            if (!f->member[0]) continue; /* the spread: below */
            snprintf(at, sizeof(at), "%s%s%s", prefix, *prefix ? "." : "", f->member);
            int field = field_named(r, at);
            if (composite(r, at)) values_under(c, r, at, f->args[0], values, label, height);
            else if (field >= 0) {
                if (values[field]) fail_at(c, f->line, f->column, "field '%s' is given twice", f->member);
                values[field] = f->args[0];
            } else fail_at(c, f->line, f->column, "record '%s' has no field '%s'", r->name, at);
        }
        return;
    }
    if (!*prefix) fail_at(c, x->line, x->column, "'%s' holds %s records, as in %s.append(%s { ... })", label, r->name, label, r->name);
    /* an item of that type: its fields of the same names (a variant's case as case) */
    size_t n = strlen(prefix);
    for (unsigned k = 0; k < r->fields; ++k) {
        const char *name = r->field_names[k];
        if (strncmp(name, prefix, n) || (name[n] && name[n] != '.')) continue;
        values[k] = member_of(c, x, name[n] ? name + n + 1 : "case");
    }
}

static void record_values_of(Compiler *c, const Expr *e, const Record *r, const char *label, const Expr **values, int height);

static void record_values(Compiler *c, const Expr *e, int list, const Expr **values, int height) {
    record_values_of(c, e, record_of(c, list), c->state_names[list], values, height);
}

/* Whether e is a call of a fn run as a function (027's 2c). */
static bool called_call(const Compiler *c, const Expr *e) {
    int module = e->kind == E_FN && *e->member ? used_library(c, c->module, e->member) : c->module;
    int fn = (e->kind == E_FN || e->kind == E_NAME) && module >= 0 ? fn_index(c, module, e->name) : -1;
    return fn >= 0 && c->fns[fn].called;
}

/* A record's value a call gives, held by a name of its own first (its fields are read one by one); e else. */
static const Expr *settled(Compiler *c, const Expr *x, int height) {
    if (!called_call(c, x)) return x;
    Kind kind = value(c, x, c->handling, height);
    if (kind.type != RECORD) fail_at(c, x->line, x->column, "'%s' gives %s, not a record's value", x->name, type_name(kind.type));
    Expr *name = node(c, E_NAME, x->line, x->column);
    snprintf(name->name, NAME, ":%u", c->settled++);
    name_value(c, name->name, kind, x->line, x->column);
    pop_named(c, local_named(c, name->name), x->line, x->column);
    return name;
}

static void record_values_of(Compiler *c, const Expr *e, const Record *r, const char *label, const Expr **values, int height) {
    if (r->scalar && e->kind != E_RECORD) { /* xs.append(5): the value itself */
        values[0] = e;
        return;
    }
    for (unsigned k = 0; k < r->fields; ++k) values[k] = NULL;
    e = settled(c, e, height);
    int which;
    unsigned index;
    const Expr *payload;
    if (r->variant && variant_case(c, e, &which, &index, &payload)) { /* xs.append(Json.num(3)) */
        values_under(c, r, "", e, values, label, height);
        return;
    }
    char shown[2 * NAME + 1];
    snprintf(shown, sizeof(shown), "%s", record_shown(c, c->module, r));
    char path[2 * NAME + 2];
    int binding = e->kind != E_RECORD ? binding_path(c, e, path) : -1;
    if (e->kind == E_NAME && binding < 0 && !c->scope) { /* an item of the same record (a walk's, an index let's): its fields */
        int loop = loop_named(c, e->name), local = local_named(c, e->name), list = -1;
        if (loop >= 0 && c->loops[loop].list >= 0) list = c->loops[loop].list;
        else if (local >= 0 && c->locals[local].kind.type == INDEX) list = c->locals[local].kind.owner;
        if (list >= 0 && record_of(c, list) == r) {
            for (unsigned k = 0; k < r->fields; ++k) values[k] = member_of(c, e, r->field_names[k]);
            return;
        }
    }
    if (binding >= 0) { /* a record's value held by name, or one under it (027's 2c): of r alone */
        bool root = !strcmp(path, c->locals[binding].name);
        int held = root ? c->locals[binding].kind.owner : sub_record(c, c->locals[binding].kind.owner, path + strlen(c->locals[binding].name) + 1);
        if (held != (int)(r - c->records))
            fail_at(c, e->line, e->column, "'%s' holds %s, not %s", label, shown, held >= 0 ? c->records[held].name : path);
    }
    if ((r->variant || binding >= 0) && e->kind != E_RECORD) { /* an item of the variant, a variant in a field, or a value held by name */
        for (unsigned k = 0; k < r->fields; ++k) values[k] = member_of(c, e, r->field_names[k]);
        return;
    }
    if (e->kind != E_RECORD)
        fail_at(c, e->line, e->column, "'%s' holds %s records, as in %s.append(%s { %s: ... })", label, shown, label, shown,
                r->field_names[0]);
    /* The record it names, as the file it is written in names it: its own, or a library's it uses. */
    int from = e->member[0] ? used_library(c, c->module, e->member) : c->module;
    if (from != r->module || strcmp(e->name, r->name))
        fail_at(c, e->line, e->column, "'%s' holds %s, not %s%s%s", label, shown, e->member, e->member[0] ? "." : "", e->name);
    values_under(c, r, "", e, values, label, height);
    const Expr *spreading = spread_of(e);
    for (unsigned k = 0; spreading && k < r->fields; ++k)
        if (!values[k]) values[k] = spread_field(c, spreading, r->field_names[k]);
}

/* A list in a field, its value onto the stack: a let's list (a copy), [] (empty), or a list value of its record. */
static void list_field(Compiler *c, const Record *r, unsigned field, const Expr *x, int height) {
    int record = r->field_record[field], named = x->kind == E_NAME && !c->module && parameter(c, x->name) < 0 ?
                                                 state_index(c, x->name) : -1;
    if (named >= 0 && c->state_types[named] == STATE_LIST) {
        int list = list_state(c, x->name, x->line, x->column, NULL);
        if (module_record(c, c->state_library[list], c->state_target[list]) != record)
            fail_at(c, x->line, x->column, "'%s' holds a list of %s, not of %s", r->field_names[field], c->records[record].name,
                    record_of(c, list)->name);
        emit(c, x->line, x->column, LIST_VALUE, (uint32_t)list, 0);
        return;
    }
    if (x->kind == E_LIST) {
        if (x->args[0]) fail_at(c, x->line, x->column, "a list in a field is given as a let's list, or [] for none");
        emit(c, x->line, x->column, LIST_VALUE, (uint32_t)hidden_list(c, record, false, x->line, x->column), 0);
        return;
    }
    Kind kind = value(c, x, c->handling, height);
    if (kind.type != LIST || kind.owner != record)
        fail_at(c, x->line, x->column, "'%s' holds a list of %s, not %s", r->field_names[field], c->records[record].name,
                kind.type == LIST ? c->records[kind.owner].name : type_name(kind.type));
}

/* A record's fields in order, onto the stack from height; missing fields take their default. */
static void record_fields_of(Compiler *c, const Expr *e, const Record *r, const Expr **values, int height) {
    for (unsigned k = 0; k < r->fields; ++k) {
        int at = height + (int)k;
        if (at + 1 > STACK) fail_at(c, e->line, e->column, "expression needs more than 64 stack values");
        if (r->field_kinds[k] == LIST) {
            if (values[k]) list_field(c, r, k, values[k], at);
            else emit(c, e->line, e->column, LIST_VALUE, (uint32_t)hidden_list(c, r->field_record[k], false, e->line, e->column), 0);
        } else if (values[k]) expect_kind(c, values[k], wanted(c, values[k], c->handling, at, field_kind(r, k)), field_kind(r, k));
        else if (r->field_kinds[k] == TEXT || r->field_kinds[k] == BYTES) {
            emit(c, e->line, e->column, TEXT_CONST, r->field_constant[k], 0);
            if (r->field_kinds[k] == BYTES) emit(c, e->line, e->column, BYTES_FROM, 0, 0);
        } else if (r->field_kinds[k] == I64) emit_integer(c, e->line, e->column, r->field_integer[k]);
        else emit(c, e->line, e->column, CONST, r->field_kinds[k] == BOOL, r->field_number[k]);
    }
}

static void record_fields(Compiler *c, const Expr *e, int list, int height) {
    const Expr *values[FIELDS_PER_RECORD];
    record_values(c, e, list, values, height);
    record_fields_of(c, e, record_of(c, list), values, height);
}

/* send's values by the output's fields, groups by their paths. */
static void send_values(Compiler *c, const Input *out, const Expr *group, const char *path, const Expr **given) {
    for (const Expr *f = group->args[0]; f; f = f->next) {
        if (!f->member[0]) { /* the spread: the send's own braces only */
            if (*path) fail_at(c, f->line, f->column, "a spread goes in the send's own braces, not a group's");
            continue;
        }
        char name[NAME];
        int length = snprintf(name, sizeof(name), "%s%s%s", path, *path ? "." : "", f->member);
        if (length < 0 || length >= NAME) fail_at(c, f->line, f->column, "output '%s' has no field '%s.%s'", out->name, path, f->member);
        bool group_named = false;
        int field = -1;
        for (unsigned i = 0; i < out->fields; ++i) {
            if (!strcmp(out->field_names[i], name)) field = (int)i;
            else if (!strncmp(out->field_names[i], name, (size_t)length) && out->field_names[i][length] == '.') group_named = true;
        }
        const Expr *v = f->args[0];
        if (v->kind == E_RECORD && !v->name[0]) {
            if (!group_named) fail_at(c, f->line, f->column, "output '%s' has no group '%s'", out->name, name);
            send_values(c, out, v, name, given);
            continue;
        }
        if (group_named) fail_at(c, f->line, f->column, "'%s' is a group of fields; give them in braces, as in %s: { ... }", name, f->member);
        if (field < 0) fail_at(c, f->line, f->column, "output '%s' has no field '%s'", out->name, name);
        if (given[field]) fail_at(c, f->line, f->column, "field '%s' is given twice", name);
        given[field] = v;
    }
}

/* A view's body changes only its lets and its own lists (a let's list): its states are a snapshot's. */
static void view_changes(Compiler *c, const Stmt *s, int list, const char *name) {
    if (!c->handling && (list < 0 || !c->state_event[list]))
        fail_at(c, s->line, s->column, "a view's body changes only its lets and its own lists, not '%s'", name);
}

/* The list a for or a fold walks, written as its name (a list, a message's rows, a let's) or as a path to a list
 * value (item.items, a match's binding): a value is bound to a register first. */
static int walked_list_at(Compiler *c, const char *path, int line, int column, int height) {
    int named = state_index(c, path), local = strchr(path, '.') ? -1 : local_named(c, path);
    if (named >= 0 || (c->module == 0 && strchr(path, '.') == NULL && alias_named(c, path) < 0 && loop_named(c, path) < 0 &&
                       parameter(c, path) < 0 && (local < 0 || c->locals[local].kind.type != LIST)))
        return list_state(c, path, line, column, NULL);
    Expr *e = path_expr(c, path, line, column);
    Kind kind = value(c, e, c->handling, height);
    if (kind.type != LIST) fail_at(c, line, column, "'%s' is not a list", path);
    return bind_list(c, e, kind);
}

/* The list a for walks or a fold folds, its path read with an empty stack below it. */
static int walked_list(Compiler *c, const char *path, int line, int column) {
    return walked_list_at(c, path, line, column, 0);
}

/* A slot for a let of bytes or text (Pure's: 0-15 bytes, 16-31 texts), the handler's next. */
static unsigned held_slot(Compiler *c, const Stmt *s, Type type) {
    unsigned *count = type == BYTES ? &c->held_bytes : &c->held_texts;
    if (*count == HELD) fail_at(c, s->line, s->column, "at most %d lets of %s are allowed in a handler", HELD, type_name(type));
    return (type == TEXT ? HELD_TEXT : 0) + (*count)++;
}

static void chars_loop(Compiler *c, const Stmt *s, const Expr *subject);
static bool text_path(Compiler *c, const char *path);

/* A let's list emptied (at a let in an inner block, or by xs = [...]): while xs.count > 0 { xs.remove(xs.last) } (a list
 * of numbers' place count - 1), the same as a new list each time the let is run. Its items go one at a time (Pure has no op that empties a list). */
static Stmt *emptied(Compiler *c, const char *name, int line, int column) {
    Stmt *loop = allocate(c, sizeof(*loop)), *check = allocate(c, sizeof(*check)), *stop = allocate(c, sizeof(*stop)),
         *remove = allocate(c, sizeof(*remove));
    Expr *list = node(c, E_NAME, line, column), *count = node(c, E_MEMBER, line, column), *none = node(c, E_NUMBER, line, column),
         *empty = node(c, E_BINARY, line, column), *last = node(c, E_MEMBER, line, column);
    memcpy(list->name, name, NAME);
    count->args[0] = last->args[0] = list;
    count->count = last->count = 1;
    memcpy(count->member, "count", 6);
    memcpy(last->member, "last", 5);
    none->whole = true;
    empty->op = LE, empty->args[0] = count, empty->args[1] = none, empty->count = 2;
    check->kind = S_IF, check->line = line, check->column = column, check->value = empty, check->then = stop;
    stop->kind = S_BREAK, stop->line = line, stop->column = column;
    remove->kind = S_REMOVE, remove->line = line, remove->column = column, remove->first = last;
    int state = state_index(c, name);
    if (state >= 0 && record_of(c, state)->scalar) { /* a list of numbers' last is its value: its place is count - 1 */
        Expr *place = node(c, E_BINARY, line, column), *one = node(c, E_NUMBER, line, column);
        one->number = 1, one->integer = 1, one->whole = true;
        place->op = SUB, place->args[0] = count, place->args[1] = one, place->count = 2;
        remove->first = place;
    }
    memcpy(remove->name, name, NAME);
    check->next = remove;
    loop->kind = S_COUNT, loop->line = line, loop->column = column;
    snprintf(loop->name, NAME, "while.%u", c->whiles++);
    loop->low = node(c, E_NUMBER, line, column);
    loop->low->whole = true;
    loop->high = node(c, E_NUMBER, line, column);
    loop->high->number = 9007199254740992.0;
    loop->high->integer = INT64_C(9007199254740992);
    loop->high->whole = true;
    loop->then = check;
    return loop;
}

static bool listed(const int *lists, unsigned n, int list) {
    for (unsigned i = 0; i < n; ++i) if (lists[i] == list) return true;
    return false;
}

/* Whether e may read what S_TOGETHER's targets change (names, of them; lists, those changed): a target's name, a fn's
 * call (it may read anything), a parameter or a case's binding (their kinds are the instance's), or an index of a
 * list changed (a let's, a for's item, a state's) through which a changed item is read. */
static bool reads_changed(Compiler *c, const Expr *e, char (*names)[NAME], unsigned n, const int *lists, unsigned l) {
    if (!e) return false;
    if (e->kind == E_FN) return true;
    for (unsigned i = 0; i < n; ++i)
        if (((e->kind == E_NAME || e->kind == E_ITEM) && !strcmp(e->name, names[i])) ||
            (e->kind == E_FOLD && !strcmp(e->member, names[i])))
            return true;
    if (e->kind == E_NAME || e->kind == E_ITEM) {
        if (!c->module && (parameter(c, e->name) >= 0 || alias_named(c, e->name) >= 0)) return true;
        if (c->module) return true;
        int local = local_named(c, e->name), loop = local < 0 ? loop_named(c, e->name) : -1,
            state = local < 0 && loop < 0 ? state_index(c, e->name) : -1;
        if (local >= 0 && c->locals[local].kind.type == INDEX && listed(lists, l, c->locals[local].kind.owner)) return true;
        if (loop >= 0 && listed(lists, l, c->loops[loop].list)) return true;
        if (state >= 0 && c->state_types[state] == STATE_INDEX && listed(lists, l, cursor_owner(c, state))) return true;
    }
    for (unsigned i = 0; i < PARAMS; ++i)
        for (const Expr *x = e->args[i]; x; x = x->next)
            if (reads_changed(c, x, names, n, lists, l)) return true;
    return false;
}

/* S_TOGETHER's targets each given its value straight, in order: when no value reads what a target before it changes
 * (then the values first, as written, is the same). */
static bool straight(Compiler *c, const Stmt *s) {
    if (s->value) return false; /* one call gives them all */
    char (*names)[NAME] = allocate(c, STACK * sizeof(*names));
    int lists[STACK];
    unsigned n = 0, l = 0;
    for (const Stmt *t = s->then; t; t = t->next) {
        if (!t->name[0]) return false; /* at.x: the list of its index is not named */
        memcpy(names[n++], t->name, NAME);
        int list = local_named(c, t->name) < 0 ? state_index(c, t->name) : -1;
        if (list >= 0 && c->state_types[list] == STATE_LIST) lists[l++] = list;
    }
    for (const Stmt *t = s->then; t; t = t->next)
        if (reads_changed(c, t->value, names, n, lists, l)) return false;
    return true;
}

/* xs = a list value (a fn's, a let's, another list's): a copy of it first (xs may be in it), then xs emptied, then
 * given the copy's items in order (added by key when xs is keyed). */
static Stmt *refilled(Compiler *c, const Stmt *s, int list) {
    Stmt *copy = allocate(c, sizeof(*copy)), *each = allocate(c, sizeof(*each)), *give = allocate(c, sizeof(*give));
    Expr *item = node(c, E_NAME, s->line, s->column);
    copy->kind = S_LET, copy->line = s->line, copy->column = s->column, copy->value = s->value, copy->refills = list + 1;
    snprintf(copy->name, NAME, "$%d$%d$copy", s->line, s->column);
    each->kind = S_FOR, each->line = s->line, each->column = s->column, each->then = give;
    snprintf(each->name, NAME, "$%d$%d$item", s->line, s->column);
    memcpy(each->member, copy->name, NAME);
    each->first = node(c, E_NAME, s->line, s->column), each->last = node(c, E_NAME, s->line, s->column); /* where they stand */
    memcpy(each->first->name, copy->name, NAME);
    memcpy(each->last->name, each->name, NAME);
    memcpy(item->name, each->name, NAME);
    give->kind = keyed_by(c, list) ? S_ADD : S_APPEND, give->line = s->line, give->column = s->column, give->value = item;
    memcpy(give->name, s->name, NAME);
    Stmt *empty = emptied(c, s->name, s->line, s->column);
    empty->next = each;
    int other = s->value->kind == E_NAME && !c->module && parameter(c, s->value->name) < 0 && local_named(c, s->value->name) < 0 ?
                state_index(c, s->value->name) : -1;
    if (other >= 0 && c->state_types[other] == STATE_LIST) { /* xs = ys: another list, walked where it is */
        if (other == list) return NULL;                     /* xs = xs: as it is */
        int record = module_record(c, c->state_library[list], c->state_target[list]),
            given = module_record(c, c->state_library[other], c->state_target[other]);
        if (record != given)
            fail_at(c, s->value->line, s->value->column, "'%s' takes a list of %s, not %s", s->name, c->records[record].name,
                    c->records[given].name);
        memcpy(each->member, s->value->name, NAME);
        memcpy(each->first->name, s->value->name, NAME);
        return empty;
    }
    copy->next = empty;
    return copy;
}

static void statements(Compiler *c, const Stmt *s) {
    unsigned scope = c->local_count; /* a block's lets end with it */
    for (const Stmt *before = NULL; s; before = s, s = s->next) {
        if (before && stops_one(before, true))
            fail_at(c, s->line, s->column, "this is never run: the line before it returns or breaks");
        switch (s->kind) {
        case S_ARM: break; /* a match's: compiled with it */
        case S_MATCH: {
            /* The item's index in a local, or a record's value held by name (027's 2c); then for each arm, if its case
             * is the one held, the arm (its binding the case's value: a field, a list, or a record's or a variant's
             * fields under it). The last arm of a match that names every case is taken without asking. */
            int list = -1, tag = -1;
            char path[2 * NAME + 2] = "", base[2 * NAME + 2];
            const Expr *subject = s->value;
            int binding = binding_path(c, subject, base);
            bool root = binding >= 0 && !strcmp(base, c->locals[binding].name);
            const Record *r = NULL, *v;
            unsigned slot = 0;
            if (binding >= 0) {
                int held = root ? c->locals[binding].kind.owner
                                : sub_record(c, c->locals[binding].kind.owner, base + strlen(c->locals[binding].name) + 1);
                if (held < 0 || !c->records[held].variant)
                    fail_at(c, subject->line, subject->column, "match takes a variant; '%s' holds none", base);
                v = &c->records[held];
            } else {
                if (!((subject->kind == E_MEMBER || subject->kind == E_ITEM ||
                       (subject->kind == E_NAME && alias_named(c, subject->name) >= 0)) &&
                      item_path(c, subject, c->handling, 0, &list, path))) {
                    Kind kind = value(c, subject, c->handling, 0);
                    if (kind.type != INDEX)
                        fail_at(c, subject->line, subject->column, "match takes an item that holds a variant, not %s", type_name(kind.type));
                    list = kind.owner;
                }
                r = record_of(c, list);
                tag = field_named(r, *path ? path : "case");
                if (tag < 0 || !r->field_variant[tag])
                    fail_at(c, subject->line, subject->column, "match takes an item that holds a variant; '%s' holds none",
                            *path ? path : r->name);
                v = &c->records[r->field_variant[tag] - 1];
                if (c->local_slots == LOCALS || c->alias_count == 16)
                    fail_at(c, s->line, s->column, "at most %d lets are allowed in a handler", LOCALS);
                slot = c->local_slots++;
                emit(c, s->line, s->column, LOCAL_STORE, slot | 1u << 8 | (uint32_t)list << 16, 0);
            }
            unsigned ends[FIELDS_PER_RECORD + 1], arms = 0, count = 0;
            bool seen[FIELDS_PER_RECORD] = {false}, rest = false, every = true;
            for (const Stmt *arm = s->then; arm; arm = arm->next) every = every && strcmp(arm->name, "_"), ++count;
            every = every && count == v->declared_count; /* the last arm then takes what is left */
            for (const Stmt *arm = s->then; arm; arm = arm->next) {
                if (rest) fail_at(c, arm->line, arm->column, "an arm after _ is never taken");
                bool other = !strcmp(arm->name, "_");
                unsigned index = 0, skip = 0;
                if (!other) {
                    while (index < v->declared_count && strcmp(v->declared[index].name, arm->name)) ++index;
                    if (index == v->declared_count)
                        fail_at(c, arm->line, arm->column, "variant '%s' has no case '%s'", v->name, arm->name);
                    if (seen[index]) fail_at(c, arm->line, arm->column, "case '%s' is matched twice", arm->name);
                    seen[index] = true;
                }
                bool asks = !other && !(every && !arm->next);
                if (asks) {
                    if (binding >= 0) {
                        char at[2 * NAME + 2];
                        field_path(v, 0, base, root, at);
                        load_named(c, local_named(c, at), arm->line, arm->column);
                    } else {
                        emit(c, arm->line, arm->column, LOCAL_LOAD, slot | 1u << 8 | (uint32_t)list << 16, 0);
                        emit(c, arm->line, arm->column, ITEM_GET, (uint32_t)list | (uint32_t)tag << 16, 0);
                    }
                    emit_integer(c, arm->line, arm->column, index);
                    emit(c, arm->line, arm->column, I64_COMPARE, EQ - LT, 0);
                    skip = c->code_count;
                    emit(c, arm->line, arm->column, JZ, 0, 0);
                }
                bool bound = *arm->member;
                unsigned names = c->local_count;
                if (bound) {
                    if (other) fail_at(c, arm->key_line, arm->key_column, "_ holds no value to name");
                    if (!v->declared[index].kind && !*v->declared[index].type)
                        fail_at(c, arm->key_line, arm->key_column, "case '%s' holds no value to name", arm->name);
                    if (declared_kind(c, arm->member) || let_named(c, arm->member) >= 0 || loop_named(c, arm->member) >= 0)
                        fail_at(c, arm->key_line, arm->key_column, "'%s' is already a name here", arm->member);
                    char at[4 * NAME];
                    snprintf(at, sizeof(at), "%s%s%s", binding >= 0 ? base : path, binding >= 0 || *path ? "." : "", arm->name);
                    if (strlen(at) >= NAME) fail_at(c, arm->line, arm->column, "a field's path '%s' passes 31 bytes", at);
                    if (binding >= 0) { /* names for what the case holds: its fields' locals, under the name */
                        int field = local_named(c, at);
                        int held = field >= 0 ? -1 : sub_record(c, (int)(v - c->records), arm->name);
                        if (field < 0 && held < 0) fail_at(c, arm->line, arm->column, "case '%s' holds nothing here", arm->name);
                        if (c->local_count == LOCALS) fail_at(c, s->line, s->column, "at most %d lets are allowed in a handler", LOCALS);
                        unsigned local = c->local_count++;
                        if (field >= 0) c->locals[local] = c->locals[field];
                        else {
                            memset(&c->locals[local], 0, sizeof(c->locals[local]));
                            c->locals[local].kind = (Kind){RECORD, held};
                            c->locals[local].list = -1;
                        }
                        memcpy(c->locals[local].name, arm->member, NAME);
                        c->locals[local].alias = true;
                        const Record *inner = held >= 0 ? &c->records[held] : NULL;
                        for (unsigned k = 0; inner && k < inner->fields; ++k) {
                            char from[2 * NAME + 2], name[2 * NAME + 2];
                            field_path(inner, k, at, false, from);
                            snprintf(name, sizeof(name), "%s.%s", arm->member, inner->field_names[k]);
                            if (strlen(name) >= NAME) fail_at(c, arm->line, arm->column, "'%s' passes 31 bytes", name);
                            if (c->local_count == LOCALS) fail_at(c, s->line, s->column, "at most %d lets are allowed in a handler", LOCALS);
                            unsigned each = c->local_count++;
                            c->locals[each] = c->locals[local_named(c, from)];
                            memcpy(c->locals[each].name, name, strlen(name) + 1);
                            c->locals[each].alias = true;
                        }
                    } else {
                        unsigned a = c->alias_count++;
                        memcpy(c->aliases[a].name, arm->member, NAME);
                        memcpy(c->aliases[a].path, at, strlen(at) + 1);
                        c->aliases[a].list = list;
                        c->aliases[a].slot = slot;
                        c->aliases[a].prefix = composite(r, c->aliases[a].path);
                    }
                }
                ++c->nested;
                statements(c, arm->then);
                --c->nested;
                if (bound && binding < 0) --c->alias_count;
                c->local_count = names;
                if (other) rest = true;
                if (arm->next && !stops(arm->then, true)) {
                    ends[arms++] = c->code_count;
                    emit(c, arm->line, arm->column, JMP, 0, 0);
                }
                if (asks) c->code[skip].arg = c->code_count;
            }
            for (unsigned i = 0; !rest && i < v->declared_count; ++i)
                if (!seen[i]) fail_at(c, s->line, s->column, "match misses case '%s': add it, or _ for the rest", v->declared[i].name);
            for (unsigned i = 0; i < arms; ++i) c->code[ends[i]].arg = c->code_count;
            break;
        }
        case S_LET: {
            if (let_named(c, s->name) >= 0) fail_at(c, s->line, s->column, "'%s' is already a let here", s->name);
            const char *kind = declared_kind(c, s->name);
            if (kind) fail_at(c, s->line, s->column, "'%s' is already declared as %s", s->name, kind);
            int loop = loop_named(c, s->name);
            if (loop >= 0)
                fail_at(c, s->line, s->column, c->loops[loop].list >= 0 ? "'%s' is the item this for walks" :
                        "'%s' is what this for counts", s->name);
            if (c->local_count == LOCALS) fail_at(c, s->line, s->column, "at most %d lets are allowed in a handler", LOCALS);
            if (s->listed) { /* the event's list: its state let.NAME, made before the handlers (event_lists) */
                if (c->folding || c->filtering) fail_at(c, s->line, s->column, "a fold or a filter holds no let's list");
                char path[2 * NAME];
                snprintf(path, sizeof(path), "let.%s", s->name);
                int list = -1;
                for (unsigned i = 0; i < c->states; ++i) if (c->state_event[i] && !strcmp(c->state_names[i], path)) list = (int)i;
                unsigned local = c->local_count++;
                memset(&c->locals[local], 0, sizeof(c->locals[local]));
                memcpy(c->locals[local].name, s->name, NAME);
                c->locals[local].list = list;
                if (c->nested) statements(c, emptied(c, s->name, s->line, s->column)); /* an inner block's: new each time */
                for (const Expr *element = s->value ? s->value->args[0] : NULL; element; element = element->next) {
                    if (keyed_by(c, list))
                        fail_at(c, element->line, element->column, "a keyed list adds its elements by key; add them one by one");
                    record_fields(c, element, list, 0);
                    emit(c, element->line, element->column, LIST_APPEND, (uint32_t)list, 0);
                }
                break;
            }
            if (c->local_slots == LOCALS) fail_at(c, s->line, s->column, "at most %d lets are allowed in a handler", LOCALS);
            Kind held = value(c, s->value, c->handling, 0);
            if (held.type == TUPLE)
                fail_at(c, s->value->line, s->value->column, "this gives several values: name each, as in let a, b = ...");
            if (s->refills) { /* xs = value: a list of xs's record */
                int list = s->refills - 1, record = module_record(c, c->state_library[list], c->state_target[list]);
                const char *name = c->state_names[list];
                if (c->state_event[list] && !strncmp(name, "let.", 4)) name += 4; /* a let's list, as written */
                if (held.type != LIST || held.owner != record)
                    fail_at(c, s->value->line, s->value->column, "'%s' takes a list of %s, not %s", name,
                            c->records[record].name, held.type == LIST ? c->records[held.owner].name : type_name(held.type));
            }
            if (held.type == RECORD || held.type == LIST) { /* a record's value, a list's (027's 2c) */
                name_value(c, s->name, held, s->line, s->column);
                pop_named(c, local_named(c, s->name), s->line, s->column);
                break;
            }
            if (held.type == BYTES || held.type == TEXT) { /* held by name in Pure: a slot of its own */
                unsigned slot = held_slot(c, s, held.type);
                unsigned local = c->local_count++;
                memset(&c->locals[local], 0, sizeof(c->locals[local]));
                memcpy(c->locals[local].name, s->name, NAME);
                c->locals[local].slot = slot;
                c->locals[local].kind = held;
                c->locals[local].list = -1;
                emit(c, s->line, s->column, BYTES_STORE, slot, 0);
                break;
            }
            if (held.type != NUM && held.type != BOOL && held.type != I64 && held.type != INDEX)
                fail_at(c, s->value->line, s->value->column, "a let holds a number, a bool, an i64, an index, bytes or text, not %s",
                        type_name(held.type));
            unsigned local = c->local_count++;
            memcpy(c->locals[local].name, s->name, NAME);
            c->locals[local].slot = c->local_slots++;
            c->locals[local].kind = held;
            c->locals[local].list = -1;
            c->locals[local].counter = false;
            emit(c, s->line, s->column, LOCAL_STORE, local_arg(c, (int)local), 0);
            break;
        }
        case S_LETS: {
            const Expr *v = s->value;
            Kind kinds[STACK];
            if (!v->next) { /* one value: a fn's call that gives them all (027's 2c) */
                Kind given = value(c, v, c->handling, 0);
                const Instance *in = given.type == TUPLE ? &c->instances[given.owner] : NULL;
                if (!in || in->result_count != s->name_count)
                    fail_at(c, in ? v->line : s->line, in ? v->column : s->column, "%u names take %u values, not %u", s->name_count,
                            s->name_count, in ? in->result_count : 1);
                memcpy(kinds, in->results, s->name_count * sizeof(*kinds));
            } else for (unsigned i = 0, height = 0; i < s->name_count; ++i, v = v->next) {
                kinds[i] = value(c, v, c->handling, (int)height);
                height += cells(c, kinds[i]);
                if (kinds[i].type != NUM && kinds[i].type != BOOL && kinds[i].type != I64 && kinds[i].type != INDEX &&
                    kinds[i].type != BYTES && kinds[i].type != TEXT && kinds[i].type != RECORD && kinds[i].type != LIST)
                    fail_at(c, v->line, v->column, "a let holds a number, a bool, an i64, an index, bytes, text, a list or a record, "
                            "not %s", type_name(kinds[i].type));
            }
            unsigned first = c->local_count;
            for (unsigned i = 0; i < s->name_count; ++i) {
                const char *name = s->names[i];
                if (let_named(c, name) >= 0) fail_at(c, s->line, s->column, "'%s' is already a let here", name);
                const char *kind = declared_kind(c, name);
                if (kind) fail_at(c, s->line, s->column, "'%s' is already declared as %s", name, kind);
                if (loop_named(c, name) >= 0) fail_at(c, s->line, s->column, "'%s' is named by a for around it", name);
                name_value(c, name, kinds[i], s->line, s->column);
            }
            (void)first;
            for (unsigned i = s->name_count; i-- > 0;) pop_named(c, local_named(c, s->names[i]), s->line, s->column);
            break;
        }
        case S_TOGETHER: {
            if (straight(c, s)) {
                statements(c, s->then);
                break;
            }
            Stmt *spread = allocate(c, sizeof(*spread)), *targets[STACK]; /* copies: a fn's body compiles for each instance */
            *spread = *s;
            spread->kind = S_LETS, spread->then = spread->next = NULL;
            spread->names = allocate(c, STACK * sizeof(*spread->names));
            Expr **link = &spread->value;
            if (s->value) { /* one call gives them all */
                *link = node(c, s->value->kind, s->value->line, s->value->column);
                **link = *s->value;
                link = &(*link)->next;
            }
            unsigned n = 0;
            for (const Stmt *t = s->then; t; t = t->next) {
                targets[n] = allocate(c, sizeof(*targets[n]));
                *targets[n] = *t;
                targets[n++]->next = NULL;
                if (t->value) {
                    *link = node(c, t->value->kind, t->value->line, t->value->column);
                    **link = *t->value;
                    link = &(*link)->next;
                }
            }
            *link = NULL;
            spread->name_count = n;
            statements(c, through_lets(c, spread, targets));
            break;
        }
        case S_ASSIGNS: {
            const Expr *v = s->value;
            int locals[STACK], states[STACK];
            bool lists = false;
            for (unsigned i = 0; i < s->name_count; ++i) {
                int list = local_named(c, s->names[i]) < 0 ? state_index(c, s->names[i]) : -1;
                lists = lists || (list >= 0 && c->state_types[list] == STATE_LIST);
            }
            if (lists) { /* xs, j = f(x): a list among the names takes its value as xs = value does, after the values */
                Stmt *spread = allocate(c, sizeof(*spread)), *targets[STACK];
                *spread = *s;
                spread->next = NULL;
                spread->names = allocate(c, STACK * sizeof(*spread->names));
                Expr **link = &spread->value;
                for (const Expr *x = s->value; x; x = x->next) { /* copies: a fn's body is compiled for each instance */
                    *link = node(c, x->kind, x->line, x->column);
                    **link = *x;
                    link = &(*link)->next;
                }
                *link = NULL;
                for (unsigned i = 0; i < s->name_count; ++i) {
                    targets[i] = allocate(c, sizeof(*targets[i]));
                    targets[i]->kind = S_ASSIGN, targets[i]->line = s->line, targets[i]->column = s->column;
                    memcpy(targets[i]->name, s->names[i], NAME);
                }
                statements(c, through_lets(c, spread, targets));
                break;
            }
            if (!v->next) { /* a fn's call that gives them all (027's 2c): each into a let of its kind */
                Kind given = value(c, v, c->handling, 0);
                const Instance *in = given.type == TUPLE ? &c->instances[given.owner] : NULL;
                if (!in || in->result_count != s->name_count)
                    fail_at(c, in ? v->line : s->line, in ? v->column : s->column, "%u names take %u values, not %u", s->name_count,
                            s->name_count, in ? in->result_count : 1);
                for (unsigned i = 0; i < s->name_count; ++i) { /* a let of its kind, or a number's, bool's or i64's state */
                    locals[i] = local_named(c, s->names[i]);
                    states[i] = locals[i] < 0 ? state_index(c, s->names[i]) : -1;
                    int type = states[i] >= 0 ? c->state_types[states[i]] : -1;
                    Kind held = locals[i] >= 0 ? c->locals[locals[i]].kind :
                                (Kind){type == STATE_NUM ? NUM : type == STATE_BOOL ? BOOL : type == STATE_I64 ? I64 : 0, -1};
                    if ((locals[i] < 0 && !held.type) || (locals[i] >= 0 && c->locals[locals[i]].counter) || !same_kind(held, in->results[i]))
                        fail_at(c, s->line, s->column, "'%s' takes %s here: a let or a state of that kind", s->names[i],
                                type_name(in->results[i].type));
                    if (states[i] >= 0) view_changes(c, s, -1, s->names[i]);
                }
                for (unsigned i = s->name_count; i-- > 0;)
                    if (locals[i] >= 0) pop_named(c, locals[i], s->line, s->column);
                    else emit(c, s->line, s->column, STORE, (uint32_t)states[i], 0);
                break;
            }
            for (unsigned i = 0; i < s->name_count; ++i, v = v->next) {
                const char *name = s->names[i];
                locals[i] = local_named(c, name);
                states[i] = locals[i] < 0 ? state_index(c, name) : -1;
                if (locals[i] >= 0 && c->locals[locals[i]].counter)
                    fail_at(c, s->line, s->column, "'%s' is what this for counts; it is not assigned", name);
                Kind want;
                if (locals[i] >= 0 && (c->locals[locals[i]].kind.type == BYTES || c->locals[locals[i]].kind.type == TEXT))
                    fail_at(c, s->line, s->column, "'%s' holds %s: it takes its value apart, as in %s = ...", name,
                            type_name(c->locals[locals[i]].kind.type), name);
                if (locals[i] >= 0) want = c->locals[locals[i]].kind;
                else if (states[i] >= 0 && (c->state_types[states[i]] == STATE_NUM || c->state_types[states[i]] == STATE_BOOL ||
                                            c->state_types[states[i]] == STATE_I64)) {
                    want = (Kind){c->state_types[states[i]] == STATE_I64 ? I64 : c->state_types[states[i]] == STATE_BOOL ? BOOL : NUM, -1};
                    view_changes(c, s, -1, name);
                } else if (states[i] >= 0) fail_at(c, s->line, s->column, "'%s' takes its value apart, as in %s = ...", name, name);
                else {
                    const char *kind = declared_kind(c, name);
                    if (kind) fail_at(c, s->line, s->column, "cannot assign to %s '%s'", kind, name);
                    fail_at(c, s->line, s->column, "unknown state '%s'", name);
                }
                Kind got = wanted(c, v, c->handling, (int)i, want.type);
                if (got.type != want.type || (got.type == INDEX && got.owner != want.owner))
                    fail_at(c, v->line, v->column, "'%s' holds %s", name, want.type == INDEX ? "an index of its list" : type_name(want.type));
            }
            for (unsigned i = s->name_count; i-- > 0;)
                if (locals[i] >= 0) emit(c, s->line, s->column, LOCAL_STORE, local_arg(c, locals[i]), 0);
                else emit(c, s->line, s->column, STORE, (uint32_t)states[i], 0);
            break;
        }
        case S_ASSIGN: {
            int local = local_named(c, s->name);
            if (local >= 0 && c->locals[local].counter)
                fail_at(c, s->line, s->column, "'%s' is what this for counts; it is not assigned", s->name);
            if (local >= 0 && (c->locals[local].kind.type == BYTES || c->locals[local].kind.type == TEXT)) {
                /* s = s + e appends e to what s holds; else s holds the value */
                const Expr *v = s->value;
                bool append = v->kind == E_BINARY && v->op == ADD && v->args[0]->kind == E_NAME && !strcmp(v->args[0]->name, s->name) &&
                              v->args[1]->kind != E_UNIT;
                const Expr *given = append ? v->args[1] : v;
                Kind kind = value(c, given, c->handling, 0);
                if (kind.type != c->locals[local].kind.type)
                    fail_at(c, given->line, given->column, "'%s' holds %s, not %s", s->name, type_name(c->locals[local].kind.type),
                            type_name(kind.type));
                emit(c, s->line, s->column, BYTES_STORE, c->locals[local].slot | (append ? HELD_APPEND : 0), 0);
                break;
            }
            if (local >= 0 && (c->locals[local].kind.type == RECORD || c->locals[local].kind.type == LIST)) { /* (027's 2c) */
                Kind kind = value(c, s->value, c->handling, 0);
                if (!same_kind(kind, c->locals[local].kind))
                    fail_at(c, s->value->line, s->value->column, "'%s' holds %s of %s", s->name, type_name(c->locals[local].kind.type),
                            c->records[c->locals[local].kind.owner].name);
                pop_named(c, local, s->line, s->column);
                break;
            }
            if (local >= 0 && c->locals[local].alias)
                fail_at(c, s->line, s->column, "'%s' names what a case holds; it is not assigned", s->name);
            if (local >= 0) { /* a let takes a new value of its kind */
                Kind kind = wanted(c, s->value, c->handling, 0, c->locals[local].kind.type);
                if (kind.type != c->locals[local].kind.type || (kind.type == INDEX && kind.owner != c->locals[local].kind.owner))
                    fail_at(c, s->value->line, s->value->column, "'%s' holds %s", s->name,
                            c->locals[local].kind.type == INDEX ? "an index of its list" : type_name(c->locals[local].kind.type));
                emit(c, s->line, s->column, LOCAL_STORE, local_arg(c, local), 0);
                break;
            }
            int index = state_index(c, s->name);
            if (index < 0) {
                const char *kind = declared_kind(c, s->name);
                if (kind) fail_at(c, s->line, s->column, "cannot assign to %s '%s'", kind, s->name);
                fail_at(c, s->line, s->column, "unknown state '%s'", s->name);
            }
            view_changes(c, s, -1, s->name);
            if (c->state_types[index] == STATE_I64) {
                integer(c, s->value, c->handling, 0);
                emit(c, s->line, s->column, STORE, (uint32_t)index, 0);
            } else if (c->state_types[index] == STATE_NUM || c->state_types[index] == STATE_BOOL) {
                Type type = c->state_types[index] == STATE_BOOL ? BOOL : NUM;
                expect_kind(c, s->value, wanted(c, s->value, c->handling, 0, type), type);
                emit(c, s->line, s->column, STORE, (uint32_t)index, 0);
            } else if (c->state_types[index] == STATE_LIST && s->value->kind == E_LIST) {
                statements(c, emptied(c, s->name, s->line, s->column)); /* xs = [...]: the list emptied, then its elements */
                if (keyed_by(c, index) && s->value->args[0])
                    fail_at(c, s->value->line, s->value->column, "a keyed list adds its elements by key; add them one by one");
                for (const Expr *element = s->value->args[0]; element; element = element->next) {
                    record_fields(c, element, index, 0);
                    emit(c, element->line, element->column, LIST_APPEND, (uint32_t)index, 0);
                }
            } else if (c->state_types[index] == STATE_LIST) {
                statements(c, refilled(c, s, index)); /* xs = a list value */
            } else if (c->state_types[index] == STATE_INDEX) {
                index_of(c, s->value, c->handling, 0, cursor_owner(c, index));
                emit(c, s->line, s->column, INDEX_STORE, (uint32_t)index, 0);
            } else if (c->state_types[index] == STATE_TEXT) {
                Type type = c->state_bytes[index] ? BYTES : TEXT;
                expect_kind(c, s->value, meeting(c, s->value, c->handling, 0, type), type);
                emit(c, s->line, s->column, TEXT_SET, (uint32_t)index, 0);
            } else {
                int owner = cursor_owner(c, index);
                cursor_of(c, s->value, c->handling, 0, owner, c->state_names[owner]);
                emit(c, s->line, s->column, CUR_STORE, (uint32_t)index, 0);
            }
            break;
        }
        case S_INSERT: case S_REMOVE: {
            int list = state_index(c, s->name);
            view_changes(c, s, list >= 0 && c->state_types[list] == STATE_LIST ? list : -1, s->name);
            if (list >= 0 && c->state_types[list] == STATE_LIST) {
                if (s->last) fail_at(c, s->last->line, s->last->column, "a list removes one item, as in %s.remove(index)", s->name);
                if (s->kind == S_INSERT && walked(c, list))
                    fail_at(c, s->line, s->column, "a for does not add to the list it walks; add to another list, or after the for");
                if (s->kind == S_REMOVE && walked(c, list) == 2)
                    fail_at(c, s->line, s->column, "a for removes from the list it walks, not from one an outer for walks");
                if (s->kind == S_INSERT && keyed_by(c, list))
                    fail_at(c, s->line, s->column, "'%s' is keyed by %s: %s.add(%s { ... }) places an item by its key", s->name,
                            keyed_by(c, list), s->name, record_shown(c, c->module, record_of(c, list)));
                if (s->kind == S_REMOVE) item_of(c, s->first, c->handling, 0, list);
                else index_of(c, s->first, c->handling, 0, list);
                if (s->kind == S_INSERT) record_fields(c, s->value, list, 1);
                emit(c, s->line, s->column, s->kind == S_INSERT ? LIST_INSERT : LIST_REMOVE, (uint32_t)list, 0);
                break;
            }
            int text = text_state(c, s->name, s->line, s->column);
            cursor_of(c, s->first, c->handling, 0, text, s->name);
            if (s->kind == S_INSERT) {
                expect_kind(c, s->value, value(c, s->value, c->handling, 1), TEXT);
                emit(c, s->line, s->column, TEXT_INSERT, (uint32_t)text, 0);
                break;
            }
            if (!s->last) fail_at(c, s->line, s->column, "a text removes a range, as in %s.remove(first .. last)", s->name);
            cursor_of(c, s->last, c->handling, 1, text, s->name);
            emit(c, s->line, s->column, TEXT_REMOVE, (uint32_t)text, 0);
            break;
        }
        case S_APPEND: case S_ADD: {
            if (!*s->name) { /* x.items.append(item): into the list in a field of an item of an event's list (027's 2b) */
                int list;
                char path[2 * NAME + 2];
                if (!item_path(c, s->first, c->handling, 0, &list, path))
                    fail_at(c, s->first->line, s->first->column, "append goes into a list, or a list in a field of an item (x.items)");
                const Record *r = record_of(c, list);
                int field = field_named(r, path);
                if (field < 0 || r->field_kinds[field] != LIST)
                    fail_at(c, s->first->line, s->first->column, "'%s' is not a list in a field", path);
                if (c->state_bound[list] || c->state_rows[list])
                    fail_at(c, s->first->line, s->first->column, "a list in a field changes in an item of a let's, a stored or a "
                            "transient list, not of a message's rows or one read from a record");
                view_changes(c, s, list, c->state_names[list]);
                const Record *inner = &c->records[r->field_record[field]];
                const Expr *values[FIELDS_PER_RECORD];
                record_values_of(c, s->value, inner, path, values, 1);
                record_fields_of(c, s->value, inner, values, 1);
                emit(c, s->line, s->column, APPEND_IN, (uint32_t)list | (uint32_t)field << 16, 0);
                break;
            }
            /* A keyed list places an added item by its key (it fails if the key is there); others append. */
            int list = list_state(c, s->name, s->line, s->column, NULL);
            view_changes(c, s, list, s->name);
            if (walked(c, list))
                fail_at(c, s->line, s->column, "a for does not add to the list it walks; add to another list, or after the for");
            const char *key = keyed_by(c, list), *record = record_shown(c, c->module, record_of(c, list));
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
            if (!s->member[0]) { /* tasks[at] = Task { ... }: each field set, the key the same */
                view_changes(c, s, list, s->name);
                const Record *r = record_of(c, list);
                const Expr *values[FIELDS_PER_RECORD];
                record_values(c, s->value, list, values, 0);
                int key = c->list_key[list] - 1;
                for (unsigned k = 0; k < r->fields; ++k)
                    if (!values[k]) fail_at(c, s->value->line, s->value->column, "%s[...] = %s { ... } needs '%s'", s->name,
                                            record_shown(c, c->module, r), r->field_names[k]);
                if (key >= 0) { /* require tasks[at].key == the value's key */
                    Expr *item = node(c, E_ITEM, s->line, s->column), *same = node(c, E_BINARY, s->line, s->column);
                    memcpy(item->name, s->name, NAME);
                    memcpy(item->member, r->field_names[key], NAME);
                    item->args[0] = s->first;
                    item->count = 1;
                    same->op = EQ;
                    same->args[0] = item;
                    same->args[1] = (Expr *)values[key];
                    same->count = 2;
                    truth(c, same, c->handling, 0, "require");
                    emit(c, s->line, s->column, REQUIRE, 0, 0);
                }
                for (unsigned k = 0; k < r->fields; ++k) {
                    if ((int)k == key) continue;
                    item_of(c, s->first, c->handling, 0, list);
                    expect_kind(c, values[k], wanted(c, values[k], c->handling, 1, field_kind(r, k)), field_kind(r, k));
                    emit(c, s->line, s->column, ITEM_SET, (uint32_t)list | k << 16, 0);
                }
                break;
            }
            if (list < 0) {
                Kind index = value(c, s->first, c->handling, 0);
                expect_kind(c, s->first, index, INDEX);
                list = index.owner;
            }
            view_changes(c, s, list, c->state_names[list]);
            const Record *r = record_of(c, list);
            unsigned field = record_field(c, r, s->member, s->line, s->column);
            if (c->list_key[list] == (int)field + 1)
                fail_at(c, s->line, s->column, "the key of '%s' does not change; remove the item and add it again", c->state_names[list]);
            if (*s->name) item_of(c, s->first, c->handling, 0, list);
            if (r->field_kinds[field] == LIST) list_field(c, r, field, s->value, 1); /* a let's list, [], or a list value */
            else expect_kind(c, s->value, wanted(c, s->value, c->handling, 1, field_kind(r, field)), field_kind(r, field));
            emit(c, s->line, s->column, ITEM_SET, (uint32_t)list | field << 16, 0);
            break;
        }
        case S_SEND: {
            if (!c->handling) fail_at(c, s->line, s->column, "a view sends nothing; a handler sends");
            int output = input_index(c, s->name);
            if (output < 0 || c->inputs[output].kind != KIND_OUTPUT) {
                const char *kind = declared_kind(c, s->name);
                if (kind) fail_at(c, s->line, s->column, "'%s' is %s, not an output", s->name, kind);
                fail_at(c, s->line, s->column, "unknown output '%s'", s->name);
            }
            const Input *out = &c->inputs[output];
            const Expr *given[FIELDS] = {0};
            send_values(c, out, s->value, "", given);
            const Expr *spreading = spread_of(s->value);
            for (unsigned f = 0; spreading && f < out->fields; ++f)
                if (!given[f]) given[f] = spread_field(c, spreading, out->field_names[f]);
            if (out->fields > STACK) fail_at(c, s->line, s->column, "an output of more than %d fields cannot be sent", STACK);
            for (unsigned f = 0; f < out->fields; ++f) {
                if (!given[f]) fail_at(c, s->line, s->column, "send %s needs '%s'", s->name, out->field_names[f]);
                int type = out->field_types[f];
                if (type == FIELD_BYTES) bytes_value(c, given[f], (int)f, "a bytes field");
                else expect_kind(c, given[f], wanted(c, given[f], c->handling, (int)f, message_kind(type)), message_kind(type));
            }
            emit(c, s->line, s->column, SEND, (uint32_t)output, 0);
            break;
        }
        case S_REPLY:
            if (c->message < 0 || c->inputs[c->message].kind != KIND_MESSAGE)
                fail_at(c, s->line, s->column, "reply is only available in a message handler");
            if (c->loop_count) fail_at(c, s->line, s->column, "a message replies once: reply after the for");
            bytes_value(c, s->value, 0, "reply");
            emit(c, s->line, s->column, REPLY, 0, 0);
            break;
        case S_REQUIRE:
            if (!c->handling) fail_at(c, s->line, s->column, "a view requires nothing; its value says what it found");
            truth(c, s->value, c->handling, 0, "require");
            emit(c, s->line, s->column, REQUIRE, 0, 0);
            break;
        case S_FOR: {
            /* EACH and the block after it, run once for every item the list has, with the item as ROW. */
            if (c->loop_count == NESTING) fail_at(c, s->line, s->column, "fors nest at most %d deep", NESTING);
            if (!s->low && !s->value && text_path(c, s->member)) { /* for ch in t: its characters (027's 2c) */
                chars_loop(c, s, path_expr(c, s->member, s->first->line, s->first->column));
                break;
            }
            int list = walked_list(c, s->member, s->first->line, s->first->column);
            const char *kind = declared_kind(c, s->name);
            if (kind) fail_at(c, s->last->line, s->last->column, "'%s' is already declared as %s", s->name, kind);
            if (let_named(c, s->name) >= 0 || loop_named(c, s->name) >= 0)
                fail_at(c, s->last->line, s->last->column, "'%s' is already a let or a for's name here", s->name);
            if (s->low) { /* a slice: the keys from low up to before high */
                Type key = keyed_by(c, list) ? field_kind(record_of(c, list), (unsigned)c->list_key[list] - 1) : TEXT;
                if (key != NUM && key != I64)
                    fail_at(c, s->first->line, s->first->column, "'%s' is not keyed by a number or an i64: a slice takes one",
                            s->member);
                expect_kind(c, s->low, wanted(c, s->low, c->handling, 0, key), key);
                expect_kind(c, s->high, wanted(c, s->high, c->handling, 1, key), key);
            }
            if (list > 0xFF) /* a walk names its list in a byte of its argument (Pure's EACH) */
                fail_at(c, s->line, s->column, "a walked list is among the first 256 states; '%s' is state %d", s->member, list);
            unsigned at = c->code_count, jump = 0, loop = c->loop_count++;
            emit(c, s->line, s->column, EACH, 0, 0);
            memcpy(c->loops[loop].name, s->name, NAME);
            c->loops[loop].list = list;
            c->loops[loop].item = -1;
            c->loops[loop].scope = c->scope;
            if (s->value) {
                truth(c, s->value, c->handling, 0, "where");
                jump = c->code_count;
                emit(c, s->line, s->column, JZ, 0, 0);
            }
            if (holds_walk(s->then)) { /* the walks within read this item from a local */
                if (c->local_slots == LOCALS) fail_at(c, s->line, s->column, "at most %d lets are allowed in a handler", LOCALS);
                c->loops[loop].item = (int)c->local_slots++;
                emit(c, s->line, s->column, ROW, (uint32_t)list, 0);
                emit(c, s->line, s->column, LOCAL_STORE, (uint32_t)c->loops[loop].item | 1u << 8 | (uint32_t)list << 16, 0);
            }
            ++c->nested;
            statements(c, s->then);
            --c->nested;
            if (s->value) c->code[jump].arg = c->code_count;
            --c->loop_count;
            bool empty = c->code_count == at + 1;
            if (empty && !s->low) c->code_count = at; /* nothing to do for any item */
            else {
                if (empty) { /* a slice's keys were computed (they fault or not): a block that does nothing */
                    emit(c, s->line, s->column, CONST, 1, 0);
                    emit(c, s->line, s->column, JZ, c->code_count + 1, 0);
                }
                c->code[at].arg = (uint32_t)list << 8 | (s->low ? 1u : 0) | (c->code_count - at - 1) << 16;
            }
            break;
        }
        case S_IF: {
            truth(c, s->value, c->handling, 0, "if");
            unsigned jump = c->code_count;
            emit(c, s->line, s->column, JZ, 0, 0);
            ++c->nested;
            statements(c, s->then);
            if (s->otherwise) {
                bool past = !stops(s->then, true); /* (a then that returns or breaks has no way past the else) */
                unsigned skip = c->code_count;
                if (past) emit(c, s->line, s->column, JMP, 0, 0);
                c->code[jump].arg = c->code_count;
                statements(c, s->otherwise);
                if (past) c->code[skip].arg = c->code_count;
            } else c->code[jump].arg = c->code_count;
            --c->nested;
            break;
        }
        case S_COUNT: {
            /* LOOP and the block after it, run once for each value from low up to before high, the value in a local:
             * numbers (whole), or i64s when either is one. */
            if (c->loop_count == NESTING) fail_at(c, s->line, s->column, "fors nest at most %d deep", NESTING);
            const char *kind = declared_kind(c, s->name);
            if (kind) fail_at(c, s->line, s->column, "'%s' is already declared as %s", s->name, kind);
            if (let_named(c, s->name) >= 0 || loop_named(c, s->name) >= 0)
                fail_at(c, s->line, s->column, "'%s' is already a let or a for's name here", s->name);
            if (c->local_slots == LOCALS || c->local_count == LOCALS)
                fail_at(c, s->line, s->column, "at most %d lets are allowed in a handler", LOCALS);
            bool integer = probe(c, s->low, c->handling, 0) == I64 || probe(c, s->high, c->handling, 0) == I64;
            Type type = integer ? I64 : NUM;
            expect_kind(c, s->low, wanted(c, s->low, c->handling, 0, type), type);
            expect_kind(c, s->high, wanted(c, s->high, c->handling, 1, type), type);
            unsigned scope = c->local_count, local = c->local_count++, loop = c->loop_count++, at = c->code_count;
            memcpy(c->locals[local].name, s->name, NAME);
            c->locals[local].slot = c->local_slots++;
            c->locals[local].kind = (Kind){type, -1};
            c->locals[local].list = -1;
            c->locals[local].counter = true;
            memcpy(c->loops[loop].name, s->name, NAME);
            c->loops[loop].list = -1;
            c->loops[loop].item = -1;
            c->loops[loop].scope = c->scope;
            emit(c, s->line, s->column, LOOP, 0, 0);
            ++c->nested;
            statements(c, s->then);
            --c->nested;
            c->code[at].arg = c->locals[local].slot | (integer ? LOOP_INTEGER : 0) | (c->code_count - at - 1) << 16;
            --c->loop_count;
            c->local_count = scope;
            break;
        }
        case S_BREAK:
            if (!c->loop_count) fail_at(c, s->line, s->column, "break ends a for; this one is outside any");
            emit(c, s->line, s->column, BREAK, 0, 0);
            break;
        case S_CALL: {
            Kind kind = value(c, s->value, c->handling, 0);
            if (kind.type != NOTHING)
                fail_at(c, s->line, s->column, "'%s' gives %s: name it, as in let x = %s(...)", s->value->name,
                        kind.type == TUPLE ? "several values" : type_name(kind.type), s->value->name);
            break;
        }
        case S_RETURN:
            if (c->current >= 0) { /* what a fn's call gives (027's 2c) */
                Instance *in = &c->instances[c->current];
                const char *name = c->fns[in->fn].name;
                Kind kinds[STACK];
                unsigned count = 0;
                int height = 0;
                for (const Expr *v = s->value; v; v = v->next) { /* each as its type, once known (a literal an i64's) */
                    Type want = in->known && count < in->result_count ? in->results[count].type : 0;
                    Kind kind = as_value(c, v, want == NUM || want == I64 ? wanted(c, v, true, height, want) : value(c, v, true, height));
                    if (kind.type == TUPLE) { /* return f(x): what it gives */
                        if (v != s->value || v->next) fail_at(c, v->line, v->column, "a call that gives several values returns alone");
                        memcpy(kinds, c->instances[kind.owner].results, c->instances[kind.owner].result_count * sizeof(*kinds));
                        count = c->instances[kind.owner].result_count;
                        break;
                    }
                    if (count == STACK) fail_at(c, v->line, v->column, "a fn gives at most %d values", STACK);
                    kinds[count++] = kind;
                    height += (int)cells(c, kind);
                    if (height > STACK) fail_at(c, v->line, v->column, "expression needs more than 64 stack values");
                }
                if (!in->known) {
                    memcpy(in->results, kinds, count * sizeof(*kinds));
                    in->result_count = count, in->known = true;
                }
                bool same = count == in->result_count;
                for (unsigned k = 0; same && k < count; ++k) same = same_kind(kinds[k], in->results[k]);
                if (!same)
                    fail_at(c, s->line, s->column, "fn '%s' gives %u value%s here (%s first), and %u before (%s first)", name, count,
                            count == 1 ? "" : "s", count ? type_name(kinds[0].type) : "none", in->result_count,
                            in->result_count ? type_name(in->results[0].type) : "none");
                emit(c, s->line, s->column, RETURN, (uint32_t)c->current, 0);
                break;
            }
            /* a view's body's value, its last line */
            if (c->handling) fail_at(c, s->line, s->column, "return gives a view's body its value; a handler returns nothing");
            if (s->next || c->nested) fail_at(c, s->line, s->column, "return is the last line of a view's body");
            const Expr *v = s->value;
            int named = v->kind == E_NAME && parameter(c, v->name) < 0 ? state_index(c, v->name) : -1;
            if (named >= 0 && c->state_types[named] == STATE_LIST) { /* a list it built: the view's rows (Pure 027's 4e) */
                int list = list_state(c, v->name, v->line, v->column, NULL);
                if (!c->state_event[list])
                    fail_at(c, v->line, v->column, "a view's body returns a list it built; a state's list is a view as it is, "
                            "as in view all = %s", v->name);
                emit(c, v->line, v->column, LIST_VALUE, (uint32_t)list, 0);
                c->returned = (Kind){LIST, (int)module_record(c, c->state_library[list], c->state_target[list])};
            } else c->returned = value(c, s->value, false, 0);
            c->has_returned = true;
            break;
        }
    }
    c->local_count = scope;
}

/* Whether a for's list names a text (027's 2c): a let's, a parameter's, a state's, or a text field of a record's value
 * held by name. */
static bool text_path(Compiler *c, const char *path) {
    if (strchr(path, '.')) { /* a record's text field, or a message's */
        char bound[2 * NAME + 2], field[NAME];
        Expr *e = path_expr(c, path, 0, 0);
        int binding = binding_path(c, e, bound), local = binding >= 0 ? local_named(c, bound) : -1, input;
        if (binding < 0 && message_path(c, e, &input, field))
            for (unsigned f = 0; f < c->inputs[input].fields; ++f)
                if (!strcmp(c->inputs[input].field_names[f], field)) return c->inputs[input].field_types[f] == FIELD_TEXT;
        return local >= 0 && c->locals[local].kind.type == TEXT;
    }
    int param = parameter(c, path), local = c->scope ? -1 : local_named(c, path), state = c->module ? -1 : state_index(c, path);
    if (param >= 0) return c->scope->kinds[param].type == TEXT;
    if (local >= 0) return c->locals[local].kind.type == TEXT;
    return state >= 0 && c->state_types[state] == STATE_TEXT && !c->state_bytes[state];
}

/* for ch in t: t held by a name of its own, then a count whose block begins if at >= end { break }, takes the character
 * at at (its code point, an i64 named ch, read only) and moves at past it (1 to 4 bytes, by the code point). */
static void chars_loop(Compiler *c, const Stmt *s, const Expr *subject) {
    int line = s->line, column = s->column;
    const char *kind = declared_kind(c, s->name);
    if (kind) fail_at(c, s->last->line, s->last->column, "'%s' is already declared as %s", s->name, kind);
    if (let_named(c, s->name) >= 0 || loop_named(c, s->name) >= 0)
        fail_at(c, s->last->line, s->last->column, "'%s' is already a let or a for's name here", s->name);
    unsigned scope = c->local_count;
    expect_kind(c, subject, value(c, subject, c->handling, 0), TEXT);
    char name[NAME];
    int locals[4];
    static const char *const parts[] = {"text", "at", "end", "n"};
    static const Type types[] = {TEXT, I64, I64, NUM};
    for (unsigned k = 0; k < 4; ++k) {
        snprintf(name, sizeof(name), ":%.24s.%s", s->name, parts[k]);
        name_value(c, name, (Kind){types[k], -1}, line, column);
        locals[k] = (int)c->local_count - 1;
    }
    name_value(c, s->name, (Kind){I64, -1}, line, column);
    int ch = (int)c->local_count - 1;
    c->locals[ch].counter = true;
    store_named(c, locals[0], line, column);
    load_named(c, locals[0], line, column);
    emit(c, line, column, TEXT_BYTES, 0, 0);
    emit(c, line, column, I64_FROM, 0, 0);
    store_named(c, locals[2], line, column);
    emit_integer(c, line, column, 0);
    store_named(c, locals[1], line, column);
    emit(c, line, column, CONST, 0, 0);
    emit(c, line, column, CONST, 0, 9007199254740992.0);
    unsigned loop = c->loop_count++, at = c->code_count;
    memcpy(c->loops[loop].name, s->name, NAME);
    c->loops[loop].list = -1;
    c->loops[loop].item = -1;
    c->loops[loop].scope = c->scope;
    emit(c, line, column, LOOP, 0, 0);
    load_named(c, locals[1], line, column); /* if at >= end { break } */
    load_named(c, locals[2], line, column);
    emit(c, line, column, I64_COMPARE, GE - LT, 0);
    unsigned skip = c->code_count;
    emit(c, line, column, JZ, 0, 0);
    emit(c, line, column, BREAK, 0, 0);
    c->code[skip].arg = c->code_count;
    load_named(c, locals[0], line, column); /* ch = the character at at */
    load_named(c, locals[1], line, column);
    emit(c, line, column, CHAR_AT, 0, 0);
    emit(c, line, column, LOCAL_STORE, local_arg(c, ch), 0);
    load_named(c, locals[1], line, column); /* at = at + its bytes */
    static const int64_t below[] = {0x80, 0x800, 0x10000};
    unsigned exits[3];
    for (unsigned k = 0; k < 3; ++k) {
        emit(c, line, column, LOCAL_LOAD, local_arg(c, ch), 0);
        emit_integer(c, line, column, below[k]);
        emit(c, line, column, I64_COMPARE, 0, 0);
        unsigned next = c->code_count;
        emit(c, line, column, JZ, 0, 0);
        emit_integer(c, line, column, (int64_t)k + 1);
        exits[k] = c->code_count;
        emit(c, line, column, JMP, 0, 0);
        c->code[next].arg = c->code_count;
    }
    emit_integer(c, line, column, 4);
    for (unsigned k = 0; k < 3; ++k) c->code[exits[k]].arg = c->code_count;
    emit(c, line, column, I64_ADD, 0, 0);
    store_named(c, locals[1], line, column);
    ++c->nested;
    statements(c, s->then);
    --c->nested;
    c->code[at].arg = c->locals[locals[3]].slot | (c->code_count - at - 1) << 16;
    --c->loop_count;
    c->local_count = scope;
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

static unsigned char *put64(unsigned char *p, uint64_t v) {
    p = put32(p, (uint32_t)v);
    return put32(p, (uint32_t)(v >> 32));
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

/* A message's rows field fills a list of its own, named by the input and the
 * field (order.lines): a state that holds rows only while its event runs. */
/* A let's list (let x: list of R) is a state of its own, let.x: an event's
 * list (Pure 018), one for the lets of that name in the handlers' own
 * blocks, which hold one record keyed one way; no two handlers of one event
 * have one, so each begins empty. */
/* Whether the statements (and the blocks in them) hold a let's list of that name. */
static bool holds_list_let(const Stmt *s, const char *name) {
    for (; s; s = s->next)
        if ((s->kind == S_LET && s->listed && !strcmp(s->name, name)) || holds_list_let(s->then, name) ||
            holds_list_let(s->otherwise, name))
            return true;
    return false;
}

/* The lets' lists of statements h's (a handler's, a view's body's or a fn's), in its inner blocks too: each name one
 * state let.NAME of one record, emptied at each inner block's let (emptied). */
static void event_lists_in(Compiler *c, unsigned h, const Stmt *s) {
    for (; s; s = s->next) {
        event_lists_in(c, h, s->then);
        event_lists_in(c, h, s->otherwise);
        if (s->kind != S_LET || !s->listed) continue;
        char path[2 * NAME];
        snprintf(path, sizeof(path), "let.%s", s->name);
        if (strlen(path) >= NAME)
            fail_at(c, s->line, s->column, "a let's list has a name of %d characters at most", NAME - 5);
        int found = -1;
        for (unsigned i = 0; i < c->states; ++i)
            if (c->state_event[i] && !strcmp(c->state_names[i], path)) found = (int)i;
        if (found >= 0) {
            if (strcmp(c->state_target[found], s->member) || c->state_library[found] != s->library ||
                strcmp(c->state_key[found], s->key))
                fail_at(c, s->line, s->column, "'%s' is a let's list of another record or key in another handler or block; "
                        "one name holds one kind of list", s->name);
            for (unsigned k = 0; h < c->handler_count && k < h; ++k) {
                if (strcmp(c->handlers[k].input, c->handlers[h].input) || c->handlers[k].edge != c->handlers[h].edge) continue;
                if (holds_list_let(c->handlers[k].body, s->name))
                    fail_at(c, s->line, s->column, "another handler of this event has a list '%s'; name this one apart", s->name);
            }
            continue;
        }
        if (c->states == STATES) fail_at(c, s->line, s->column, "at most %d states are allowed, a let's list among them", STATES);
        unsigned i = c->states++;
        memcpy(c->state_names[i], path, strlen(path) + 1);
        c->state_types[i] = STATE_LIST;
        c->state_event[i] = true;
        memcpy(c->state_target[i], s->member, NAME);
        c->state_library[i] = s->library;
        memcpy(c->state_key[i], s->key, NAME);
        c->state_key_line[i] = s->key_line;
        c->state_key_column[i] = s->key_column;
        c->state_line[i] = s->line;
        c->state_column[i] = s->column;
    }
}

static void event_lists(Compiler *c) {
    for (unsigned h = 0; h < c->handler_count + c->views + c->fn_count; ++h) /* the handlers', the views' bodies', the fns' */
        event_lists_in(c, h, h < c->handler_count ? c->handlers[h].body : h < c->handler_count + c->views ?
                                 c->view_body[h - c->handler_count] : c->fns[h - c->handler_count - c->views].block);
}

static void rows_lists(Compiler *c) {
    for (unsigned i = 0; i < c->input_count; ++i) {
        Input *in = &c->inputs[i];
        for (unsigned f = 0; f < in->fields; ++f) {
            if (in->field_types[f] != FIELD_ROWS) continue;
            char name[2 * NAME + 2];
            int length = snprintf(name, sizeof(name), "%s.%s", in->name, in->field_names[f]);
            if (length < 0 || length >= NAME)
                fail_at(c, in->field_line[f], in->field_column[f], "'%s' is longer than %d characters", name, NAME - 1);
            if (c->states == STATES)
                fail_at(c, in->field_line[f], in->field_column[f], "at most %d states are allowed (a message's rows take one)", STATES);
            unsigned index = c->states++;
            memcpy(c->state_names[index], name, (size_t)length + 1);
            memcpy(c->state_target[index], in->field_record[f], NAME);
            c->state_library[index] = in->field_library[f];
            c->state_types[index] = STATE_LIST;
            c->state_line[index] = in->field_line[f];
            c->state_column[index] = in->field_column[f];
            c->state_rows[index] = (int)i + 1;
            in->field_list[f] = (int)index;
        }
    }
}

/* The libraries' records the app's lists and rows fields hold go into the
 * module after the app's own: library by library as they were read, each
 * in its declaration order (so records moved from the top of an app into
 * a library it uses first leave the module as it was). In a module the
 * records share one namespace with the states, inputs and views (Pure's),
 * however they were written. */
static void pull_records(Compiler *c) {
    for (unsigned i = 0; i < c->library_record_count; ++i) {
        const Record *r = &c->library_records[i];
        if (!r->used) continue;
        const char *library = c->modules[r->module].name;
        int other = record_index(c, r->name), input = input_index(c, r->name);
        const char *taken = other >= 0 ? NULL : state_index(c, r->name) >= 0 ? "a state" :
                            input >= 0 ? (c->inputs[input].kind == KIND_OUTPUT ? "an output" : "an input") : NULL;
        for (unsigned v = 0; !taken && other < 0 && v < c->views; ++v)
            if (!c->view_table[v] && !strcmp(c->view_names[v], r->name)) taken = "a view";
        if (other >= 0)
            fail_at(c, r->used_line, r->used_column, "%s.%s is '%s' in the module, as %s is: two records cannot share a name",
                    library, r->name, r->name, record_shown(c, 0, &c->records[other]));
        if (taken)
            fail_at(c, r->used_line, r->used_column, "%s.%s is '%s' in the module, and '%s' is %s here", library, r->name,
                    r->name, r->name, taken);
        if (c->record_count == RECORDS)
            fail_at(c, r->used_line, r->used_column, "at most %d records are allowed (%s.%s is one more)", RECORDS, library, r->name);
        if (c->record_fields + r->fields > RECORD_FIELDS)
            fail_at(c, r->used_line, r->used_column, "at most %d record fields are allowed (%s.%s has %u more)", RECORD_FIELDS, library,
                    r->name, r->fields);
        c->records[c->record_count++] = *r;
        c->record_fields += r->fields;
    }
}

static int scalar_record(Compiler *c, const char *name, int line, int column);

/* ---- Records holding records, variants and lists (Pure 027's 2b): flattened into paths once every one is declared */

static void flatten(Compiler *c, unsigned index);

/* The record a type names: the app's record or variant, or a list of a scalar's (list of i64). */
static int typed_record(Compiler *c, const char *type, bool list, int line, int column) {
    int found = module_record(c, 0, type);
    if (found < 0 && list) found = scalar_record(c, type, line, column);
    if (found < 0) fail_at(c, line, column, "unknown record or variant '%s'", type);
    return found;
}

/* One field of record r at path, or a record's or variant's flattened under it (a variant's case as path itself). */
static void add_flat(Compiler *c, Record *r, const char *path, Type kind, const char *type, double number, int64_t integer,
                     Expr *string, int line, int column) {
    if (strlen(path) >= NAME) fail_at(c, line, column, "a field's path '%s' passes 31 bytes", path);
    if (*type && kind != LIST) { /* a record or a variant: its fields under the path */
        int held = typed_record(c, type, false, line, column);
        flatten(c, (unsigned)held);
        const Record *inner = &c->records[held];
        for (unsigned k = 0; k < inner->fields; ++k) {
            char sub[2 * NAME + 2];
            if (inner->field_variant[k] && !strcmp(inner->field_names[k], "case")) snprintf(sub, sizeof(sub), "%s", path); /* its case */
            else snprintf(sub, sizeof(sub), "%s.%s", path, inner->field_names[k]);
            if (inner->field_kinds[k] == LIST) {
                if (strlen(sub) >= NAME) fail_at(c, line, column, "a field's path '%s' passes 31 bytes", sub);
                if (r->fields == FIELDS_PER_RECORD)
                    fail_at(c, r->line, r->column, "'%s' flattens into more than 16 fields", r->name);
                unsigned f = r->fields++;
                memcpy(r->field_names[f], sub, strlen(sub) + 1);
                r->field_kinds[f] = LIST;
                r->field_record[f] = inner->field_record[k];
                continue;
            }
            add_flat(c, r, sub, inner->field_kinds[k], "", inner->field_number[k], inner->field_integer[k], inner->field_string[k],
                     line, column);
            r->field_variant[r->fields - 1] = inner->field_variant[k]; /* a nested variant's case stays one */
        }
        return;
    }
    if (r->fields == FIELDS_PER_RECORD) fail_at(c, r->line, r->column, "'%s' flattens into more than 16 fields", r->name);
    unsigned f = r->fields++;
    memcpy(r->field_names[f], path, strlen(path) + 1);
    r->field_kinds[f] = kind;
    r->field_number[f] = number;
    r->field_integer[f] = integer;
    r->field_string[f] = string;
    r->field_record[f] = kind == LIST ? typed_record(c, type, true, line, column) : -1;
}

/* A record's or a variant's fields from what was written: a variant's first, case, the one it holds (an i64), then
 * each case's value at the case's name. */
static void flatten(Compiler *c, unsigned index) {
    Record *r = &c->records[index];
    if (r->flattened) return;
    if (r->flattening) fail_at(c, r->line, r->column, "'%s' holds itself: hold it in a list (list of %s)", r->name, r->name);
    r->flattening = true;
    r->fields = 0;
    if (r->variant) {
        add_flat(c, r, "case", I64, "", 0, 0, NULL, r->line, r->column);
        r->field_variant[0] = (int)index + 1;
    }
    for (unsigned i = 0; i < r->declared_count; ++i)
        if (!r->variant || r->declared[i].kind || *r->declared[i].type)
            add_flat(c, r, r->declared[i].name, r->declared[i].kind ? r->declared[i].kind : NUM, r->declared[i].type,
                     r->declared[i].number, r->declared[i].integer, r->declared[i].string, r->declared[i].line, r->declared[i].column);
    if (c->record_fields + r->fields > RECORD_FIELDS) fail_at(c, r->line, r->column, "at most %d record fields are allowed", RECORD_FIELDS);
    c->record_fields += r->fields;
    r->flattening = false;
    r->flattened = true;
}

static void flatten_records(Compiler *c) {
    for (unsigned i = 0; i < c->record_count; ++i) flatten(c, i);
}

/* The record a list of i64 (number, bool, text) holds: one field, value, made once the app holds such a list; -1 for
 * another name. */
static int scalar_record(Compiler *c, const char *name, int line, int column) {
    static const struct { const char *name; Type kind; } scalars[] = {{"i64", I64}, {"number", NUM}, {"bool", BOOL}, {"text", TEXT}};
    for (unsigned k = 0; k < sizeof(scalars) / sizeof(*scalars); ++k) {
        if (strcmp(name, scalars[k].name)) continue;
        int found = module_record(c, 0, name);
        if (found >= 0) return found;
        if (c->record_count == RECORDS) fail_at(c, line, column, "at most %d records are allowed, a list of %s among them", RECORDS, name);
        if (c->record_fields == RECORD_FIELDS) fail_at(c, line, column, "at most %d record fields are allowed", RECORD_FIELDS);
        Record *r = &c->records[c->record_count];
        memset(r, 0, sizeof(*r));
        memcpy(r->name, name, strlen(name) + 1);
        memcpy(r->field_names[0], "value", 6);
        r->field_kinds[0] = scalars[k].kind;
        r->fields = 1;
        r->scalar = r->flattened = true;
        r->line = line, r->column = column;
        ++c->record_fields;
        return (int)c->record_count++;
    }
    return -1;
}

static void generate(Compiler *c, unsigned char **module, size_t *size) {
    uint32_t events[HANDLERS], state_ref[STATES] = {0};
    int handler_input[HANDLERS];
    rows_lists(c);
    event_lists(c);
    pull_records(c);
    flatten_records(c);
    called_fns(c);
    for (unsigned i = 0; i < c->states; ++i) {
        if (c->state_types[i] == STATE_TEXT) {
            Expr empty = {.kind = E_TEXT, .line = c->state_line[i], .column = c->state_column[i]};
            state_ref[i] = constant(c, c->state_text[i] ? c->state_text[i] : &empty);
        } else if (c->state_types[i] == STATE_CURSOR || c->state_types[i] == STATE_INDEX) {
            bool cursor = c->state_types[i] == STATE_CURSOR;
            int target = state_index(c, c->state_target[i]);
            if (target < 0 || c->state_types[target] != (cursor ? STATE_TEXT : STATE_LIST) || (cursor && c->state_bytes[target]))
                fail_at(c, c->state_line[i], c->state_column[i], cursor ? "a cursor moves in a text state; '%s' is not one" :
                        "an index points into a list state; '%s' is not one", c->state_target[i]);
            state_ref[i] = (uint32_t)target;
            if (!c->state_transient[i] && c->state_transient[target])
                fail_at(c, c->state_line[i], c->state_column[i], "'%s' is stored but points into transient '%s'; make it "
                        "transient too", c->state_names[i], c->state_target[i]);
        } else if (c->state_types[i] == STATE_LIST) {
            int record = module_record(c, c->state_library[i], c->state_target[i]);
            if (record < 0 && !c->state_library[i])
                record = scalar_record(c, c->state_target[i], c->state_line[i], c->state_column[i]);
            for (unsigned l = 1; record < 0 && l < c->module_count; ++l) /* a library's, written without its name */
                if (c->modules[0].uses[l] && library_record(c, (int)l, c->state_target[i]) >= 0)
                    fail_at(c, c->state_line[i], c->state_column[i], "'%s' is library %s's record: write %s.%s",
                            c->state_target[i], c->modules[l].name, c->modules[l].name, c->state_target[i]);
            if (record < 0)
                fail_at(c, c->state_line[i], c->state_column[i], "a list holds a record; '%s' is not one", c->state_target[i]);
            state_ref[i] = (uint32_t)record;
            if (*c->state_key[i]) {
                const Record *r = &c->records[record];
                c->list_key[i] = 1 + (int)record_field(c, r, c->state_key[i], c->state_key_line[i], c->state_key_column[i]);
                int kind = r->field_kinds[c->list_key[i] - 1];
                if (kind == BOOL || kind == BYTES || kind == LIST)
                    fail_at(c, c->state_key_line[i], c->state_key_column[i], "a list is keyed by a number or text; '%s' is %s",
                            c->state_key[i], kind == BOOL ? "a bool" : kind == BYTES ? "bytes" : "a list");
            }
        }
    }
    for (unsigned i = 0; i < c->record_count; ++i) {
        Record *r = &c->records[i];
        for (unsigned k = 0; k < r->fields; ++k) {
            Expr empty = {.kind = E_TEXT, .line = r->line, .column = r->column};
            if (r->field_kinds[k] == TEXT || r->field_kinds[k] == BYTES)
                r->field_constant[k] = constant(c, r->field_string[k] ? r->field_string[k] : &empty);
        }
    }
    /* The messages a module has where the app wrote none (027's 4d): a key's release for an on k.up, a frame for an
     * on frame, and a pointing for a T_focus view showing an index's position, its handler moving the index. */
    unsigned written = c->input_count;
    for (unsigned i = 0, count = c->handler_count; i < count; ++i) {
        int input = *c->handlers[i].input ? input_index(c, c->handlers[i].input) : -1;
        bool release = input >= 0 && c->inputs[input].kind == KIND_KEY && c->handlers[i].edge > 0;
        if (!release && *c->handlers[i].input) continue;
        char name[NAME];
        snprintf(name, sizeof(name), release ? "%s.up" : "frame", c->handlers[i].input);
        if (input_index(c, name) < 0) {
            if (c->input_count == INPUTS) fail_at(c, c->handlers[i].line, c->handlers[i].column, "at most %d inputs are allowed", INPUTS);
            Input *in = &c->inputs[c->input_count++];
            memset(in, 0, sizeof(*in));
            memcpy(in->name, name, NAME);
            in->kind = release ? KIND_RELEASE : KIND_FRAME;
            if (release) memcpy(in->key, c->inputs[input].key, NAME);
            else memcpy(in->field_names[0], "dt", 3), in->field_types[0] = FIELD_NUMBER, in->fields = 1;
        }
        memcpy(c->handlers[i].input, name, NAME);
        c->handlers[i].edge = -1;
    }
    for (unsigned v = 0; v < c->views; ++v) {
        size_t length = strlen(c->view_names[v]);
        const Expr *e = c->view_value[v];
        if (c->view_table[v] || length <= 6 || strcmp(c->view_names[v] + length - 6, "_focus") || !e || e->kind != E_MEMBER ||
            strcmp(e->member, "position") || e->args[0]->kind != E_NAME) continue;
        int anchor = state_index(c, e->args[0]->name), table = -1;
        for (unsigned t = 0; t < c->views; ++t) { /* a list view over a list state: each ..., or the state as it is */
            const Expr *whole = c->view_value[t] && !c->view_body[t] && c->view_value[t]->kind == E_NAME ? c->view_value[t] : NULL;
            int listed = whole ? state_index(c, whole->name) : -1;
            if (!c->view_table[t] && (*c->view_list[t] || (listed >= 0 && c->state_types[listed] == STATE_LIST)) &&
                strlen(c->view_names[t]) == length - 6 && !memcmp(c->view_names[t], c->view_names[v], length - 6))
                table = (int)t;
        }
        if (anchor < 0 || c->state_types[anchor] != STATE_INDEX || table < 0) continue;
        if (c->input_count == INPUTS) fail_at(c, c->view_line[v][0], c->view_column[v][0], "at most %d inputs are allowed", INPUTS);
        if (c->handler_count == HANDLERS) fail_at(c, c->view_line[v][0], c->view_column[v][0], "at most %d handlers are allowed", HANDLERS);
        Input *in = &c->inputs[c->input_count++];
        memset(in, 0, sizeof(*in));
        char point[NAME + 8];
        snprintf(point, sizeof(point), "%s.point", c->view_names[table]);
        if (strlen(point) >= NAME) fail_at(c, c->view_line[v][0], c->view_column[v][0], "'%s' passes 31 bytes", point);
        memcpy(in->name, point, strlen(point) + 1);
        in->kind = KIND_POINT;
        in->view = (unsigned)table;
        in->list = cursor_owner(c, anchor);
        in->anchor = anchor;
        memcpy(in->field_names[0], "position", 9);
        in->field_types[0] = FIELD_U32;
        in->fields = 1;
        unsigned h = c->handler_count++;
        memcpy(c->handlers[h].input, in->name, NAME);
        c->handlers[h].edge = -1;
        c->handlers[h].line = c->view_line[v][0], c->handlers[h].column = c->view_column[v][0];
        c->handlers[h].body = NULL;
    }
    (void)written;
    for (unsigned i = 0; i < c->handler_count; ++i) {
        const char *name = c->handlers[i].input;
        int line = c->handlers[i].line, column = c->handlers[i].column;
        int input = input_index(c, name);
        if (input < 0) {
            const char *kind = declared_kind(c, name);
            if (kind) fail_at(c, line, column, "'%s' is %s, not an input", name, kind);
            fail_at(c, line, column, "unknown input '%s'", name);
        }
        if (c->inputs[input].kind == KIND_OUTPUT)
            fail_at(c, line, column, "'%s' is an output: the app sends it (send %s { ... }) and does not handle it", name, name);
        const char *what = c->inputs[input].kind == KIND_MESSAGE ? "message" :
                           c->inputs[input].kind == KIND_TIMER ? "timer" : "text input";
        bool device = c->inputs[input].kind == KIND_RELEASE || c->inputs[input].kind == KIND_FRAME ||
                      c->inputs[input].kind == KIND_POINT; /* made above */
        if (c->inputs[input].kind != KIND_KEY && !device && c->handlers[i].edge >= 0)
            fail_at(c, line, column, "'%s' is a %s; write on %s { }", name, what, name);
        if (c->inputs[input].kind == KIND_KEY && c->handlers[i].edge < 0)
            fail_at(c, line, column, "'%s' is a key input; write on %s.down or on %s.up", name, name, name);
        handler_input[i] = input;
        events[i] = (uint32_t)input; /* Pure's handler takes its input's messages (027's 4d) */
    }
    /* A view's argument names nothing else the view reads: not a state or
     * another declaration, not its table's row. */
    for (unsigned i = 0; i < c->views; ++i) {
        if (!c->view_arguments[i]) continue;
        const Input *in = &c->inputs[c->view_arguments[i] - 1];
        for (unsigned f = 0; f < in->fields; ++f) {
            const char *kind = declared_kind(c, in->field_names[f]);
            if (kind) fail_at(c, in->field_line[f], in->field_column[f], "'%s' is already declared as %s", in->field_names[f], kind);
            if (!c->view_value[i] && !strcmp(c->view_row[i], in->field_names[f]))
                fail_at(c, in->field_line[f], in->field_column[f], "'%s' names the table's row", in->field_names[f]);
        }
    }
    /* Views, then handlers, each as one contiguous block. A table names its
     * list in place of code; its columns may read its row. */
    c->message = -1;
    for (unsigned i = 0; i < c->views; ++i) {
        const Expr *named = c->view_value[i] && !c->view_body[i] && !c->view_table[i] ? c->view_value[i] : NULL;
        int whole = named && named->kind == E_NAME && !c->view_arguments[i] ? state_index(c, named->name) : -1;
        if (whole >= 0 && c->state_types[whole] != STATE_LIST) whole = -1;
        if (!c->view_value[i] || whole >= 0) { /* a list view over a list state: a map, or its items as they are */
            int list = whole >= 0 ? list_state(c, named->name, named->line, named->column, NULL)
                                  : list_state(c, c->view_list[i], c->view_line[i][1], c->view_column[i][1], NULL);
            if (whole < 0) {
                const char *row = c->view_row[i], *kind = declared_kind(c, row);
                if (kind) fail_at(c, c->view_line[i][0], c->view_column[i][0], "'%s' is already declared as %s", row, kind);
            }
            c->view_types[i] = VIEW_LIST;
            c->view_source[i] = list + 1;
            c->view_record[i] = whole >= 0 || c->view_whole[i] ? (int)module_record(c, c->state_library[list], c->state_target[list]) + 1 : 0;
            c->view_code[i][0] = c->view_code[i][1] = 0;
            if (c->view_filter[i]) {
                /* The filter runs as a column of the list: it reads the table's row. */
                c->row = (int)i;
                c->arguments = c->view_arguments[i] - 1;
                c->filtering = true;
                c->view_code[i][0] = c->code_count;
                truth(c, c->view_filter[i], false, 0, "where");
                c->view_code[i][1] = c->code_count - c->view_code[i][0];
                c->filtering = false;
            }
            continue;
        }
        c->row = c->view_table[i] - 1;
        c->arguments = c->view_arguments[c->row >= 0 ? (unsigned)c->row : i] - 1;
        c->view_code[i][0] = c->code_count;
        Kind kind;
        if (c->view_body[i]) { /* statements as a handler's, changing only its lets and lists, and the value returned */
            c->local_count = c->local_slots = c->loop_count = c->nested = c->held_bytes = c->held_texts = 0;
            c->handling = c->has_returned = false;
            statements(c, c->view_body[i]);
            kind = c->returned;
        } else kind = value(c, c->view_value[i], false, 0);
        if (kind.type != NUM && kind.type != BOOL && kind.type != I64 && kind.type != TEXT && kind.type != BYTES && kind.type != LIST)
            fail_at(c, c->view_value[i]->line, c->view_value[i]->column,
                    "a %s is a number, a bool, an i64, text, bytes or a list, not %s", c->row >= 0 ? "column" : "view",
                    type_name(kind.type));
        static const int types[] = {[NUM] = VIEW_NUM, [TEXT] = VIEW_TEXT, [BOOL] = VIEW_BOOL, [I64] = VIEW_I64, [LIST] = VIEW_LIST,
                                    [BYTES] = VIEW_BYTES};
        c->view_types[i] = types[kind.type];
        c->view_record[i] = kind.type == LIST ? kind.owner + 1 : 0; /* a list it leaves: its items' record */
        c->view_code[i][1] = c->code_count - c->view_code[i][0];
    }
    c->row = -1;
    c->arguments = -1;
    c->fold_list = -1;
    c->handling = true;
    for (unsigned i = 0; i < c->handler_count; ++i) {
        int input = handler_input[i];
        c->message = input >= 0 && c->inputs[input].kind != KIND_KEY && c->inputs[input].kind != KIND_RELEASE ? input : -1;
        c->local_count = c->local_slots = c->loop_count = c->nested = c->held_bytes = c->held_texts = 0;
        c->handler_code[i][0] = c->code_count;
        if (c->inputs[input].kind == KIND_POINT) { /* a pointing: its list's item at the row's position, the index's */
            const Input *in = &c->inputs[input];
            emit(c, c->handlers[i].line, c->handlers[i].column, FIELD, 0, 0);
            emit(c, c->handlers[i].line, c->handlers[i].column, INDEX_AT, (uint32_t)in->list, 0);
            emit(c, c->handlers[i].line, c->handlers[i].column, INDEX_STORE, (uint32_t)in->anchor, 0);
        } else statements(c, c->handlers[i].body);
        c->handler_code[i][1] = c->code_count - c->handler_code[i][0];
    }
    /* The functions' code after the handlers' and views' (027's 2c), each one's jumps moved with it. */
    unsigned signature_items = 0;
    for (unsigned i = 0; i < c->instance_count; ++i) {
        Instance *in = &c->instances[i];
        if (c->code_count + in->count > CODE) fail_at(c, 1, 1, "program exceeds 65535 instructions");
        in->start = c->code_count;
        for (unsigned k = 0; k < in->count; ++k) {
            Op op = in->ops[k];
            if (op.op == JZ || op.op == JMP) op.arg += in->start;
            c->ops[c->code_count++] = op;
        }
        unsigned taken = 0, given = 0;
        for (unsigned k = 0; k < in->param_count; ++k) taken += cells(c, in->params[k]);
        for (unsigned k = 0; k < in->result_count; ++k) given += cells(c, in->results[k]);
        if (taken > STACK || given > STACK)
            fail_at(c, c->fns[in->fn].line, c->fns[in->fn].column, "fn '%s' takes or gives more than 64 values", c->fns[in->fn].name);
        signature_items += taken + given;
    }
    static unsigned char strings[(STATES + 2 * INPUTS + FIELDS + VIEWS + RECORDS + RECORD_FIELDS) * NAME];
    size_t used = 0;
    uint32_t state_name[STATES] = {0}, input_name[INPUTS] = {0}, input_key[INPUTS] = {0};
    uint32_t field_name[FIELDS] = {0}, view_name[VIEWS] = {0}, record_name[RECORDS] = {0};
    uint32_t record_field_name[RECORDS][FIELDS_PER_RECORD] = {{0}};
    unsigned fields = 0;
    for (unsigned i = 0; i < c->states; ++i) {
        state_name[i] = intern(strings, &used, c->state_names[i]);
        if (c->state_hidden[i]) state_ref[i] = (uint32_t)c->state_record[i]; /* made while the code was */
    }
    for (unsigned i = 0; i < c->input_count; ++i) {
        input_name[i] = intern(strings, &used, c->inputs[i].name);
        input_key[i] = c->inputs[i].kind == KIND_KEY || c->inputs[i].kind == KIND_RELEASE ? intern(strings, &used, c->inputs[i].key) : 0;
        for (unsigned f = 0; f < c->inputs[i].fields; ++f)
            field_name[fields++] = intern(strings, &used, c->inputs[i].field_names[f]);
    }
    for (unsigned i = 0; i < c->views; ++i) view_name[i] = intern(strings, &used, c->view_names[i]);
    for (unsigned i = 0; i < c->record_count; ++i) {
        record_name[i] = intern(strings, &used, c->records[i].name);
        for (unsigned k = 0; k < c->records[i].fields; ++k)
            record_field_name[i][k] = intern(strings, &used, c->records[i].field_names[k]);
    }
    *size = 38 + 16u * c->states + 16u * c->input_count + 8u * fields + 8u * c->constants +
            8u * c->record_count + 16u * c->record_fields + 20u * c->views + 12u * c->handler_count + 12u * c->instance_count +
            4u * signature_items + 16u * c->code_count + used + c->constant_used;
    unsigned char *p = *module = allocate(c, *size);
    memcpy(p, "PUREMOD7", 8);
    put16(p + 8, c->states);
    put16(p + 10, c->input_count);
    put16(p + 12, c->views);
    put16(p + 14, c->handler_count);
    put32(p + 16, c->code_count);
    put32(p + 20, (uint32_t)used);
    put16(p + 24, fields);
    put16(p + 26, c->constants);
    put16(p + 28, c->record_count);
    put32(p + 30, (uint32_t)c->constant_used);
    put16(p + 34, c->record_fields);
    put16(p + 36, c->instance_count);
    p += 38;
    for (unsigned i = 0; i < c->states; ++i) {
        p = put32(p, state_name[i]);
        /* Pure's types in a module: a cursor and an index are its anchor (027's 4c), a bool and an i64 then 4 and 5. */
        static const uint32_t module_types[] = {[STATE_NUM] = 0, [STATE_TEXT] = 1, [STATE_CURSOR] = 2, [STATE_LIST] = 3,
                                                [STATE_INDEX] = 2, [STATE_BOOL] = 4, [STATE_I64] = 5};
        uint32_t type = c->state_types[i] == STATE_TEXT && c->state_bytes[i] ? 6 : module_types[c->state_types[i]]; /* 6 bytes */
        p = put32(p, type | (c->state_transient[i] ? STATE_TRANSIENT : 0));
        if (c->state_types[i] == STATE_NUM || c->state_types[i] == STATE_BOOL) p = put_double(p, c->initial[i]);
        else if (c->state_types[i] == STATE_I64) p = put64(p, (uint64_t)c->state_integer[i]);
        else { /* flags: a mark (1: the cursor stays), or a keyed list's key field + 1 */
            p = put32(p, state_ref[i]);
            p = put32(p, c->state_types[i] == STATE_LIST ? (c->state_rows[i] || c->state_event[i] ? LIST_ROWS : 0) |
                                                           (c->state_bound[i] ? LIST_BOUND : 0) | (uint32_t)c->list_key[i] :
                         c->state_stays[i] ? 1 : 0);
        }
    }
    unsigned first = 0;
    for (unsigned i = 0; i < c->input_count; ++i) {
        const Input *in = &c->inputs[i];
        p = put32(p, input_name[i]);
        /* Pure's kinds (1 a message, 2 an output, 3 arguments) and bindings (1 a press, 2 a release, 3 text, 4 a timer,
         * 5 a frame, 6 a pointing), and what a binding names: a key, a period, a table. */
        static const unsigned char kinds[] = {[KIND_KEY] = 1, [KIND_MESSAGE] = 1, [KIND_TEXT] = 1, [KIND_TIMER] = 1,
                                              [KIND_OUTPUT] = 2, [KIND_ARGUMENTS] = 3, [KIND_RELEASE] = 1, [KIND_FRAME] = 1,
                                              [KIND_POINT] = 1};
        static const unsigned char bindings[] = {[KIND_KEY] = 1, [KIND_RELEASE] = 2, [KIND_TEXT] = 3, [KIND_TIMER] = 4,
                                                 [KIND_FRAME] = 5, [KIND_POINT] = 6};
        *p++ = kinds[in->kind];
        *p++ = bindings[in->kind];
        p = put16(p, 0);
        p = put32(p, in->kind == KIND_TIMER ? in->period : in->kind == KIND_ARGUMENTS || in->kind == KIND_POINT ? in->view : input_key[i]);
        p = put16(p, first);
        p = put16(p, in->fields);
        first += in->fields;
    }
    fields = 0;
    for (unsigned i = 0; i < c->input_count; ++i)
        for (unsigned f = 0; f < c->inputs[i].fields; ++f) {
            p = put32(p, field_name[fields++]);
            /* a rows field names the list its rows fill */
            p = put32(p, (uint32_t)c->inputs[i].field_types[f] |
                             (c->inputs[i].field_types[f] == FIELD_ROWS ? (uint32_t)c->inputs[i].field_list[f] << 8 : 0));
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
            static const uint32_t types[] = {[NUM] = 0, [TEXT] = 1, [BOOL] = 2, [I64] = 3, [LIST] = 4, [BYTES] = 5}; /* Pure's */
            p = put32(p, types[r->field_kinds[k]]);
            if (r->field_kinds[k] == TEXT || r->field_kinds[k] == BYTES) { p = put32(p, r->field_constant[k]); p = put32(p, 0); }
            else if (r->field_kinds[k] == I64) p = put64(p, (uint64_t)r->field_integer[k]);
            else if (r->field_kinds[k] == LIST) p = put64(p, (uint64_t)r->field_record[k]); /* the record of its items */
            else p = put_double(p, r->field_number[k]);
        }
    for (unsigned i = 0; i < c->views; ++i) { /* 20 bytes: type, 0, table + 1, record + 1, list + 1, block */
        p = put32(p, view_name[i]);
        *p++ = (unsigned char)c->view_types[i];
        *p++ = 0;
        p = put16(p, (uint32_t)c->view_table[i]);
        p = put16(p, (uint32_t)c->view_record[i]);
        p = put16(p, (uint32_t)c->view_source[i]);
        p = put32(p, c->view_code[i][0]);
        p = put32(p, c->view_code[i][1]);
    }
    for (unsigned i = 0; i < c->handler_count; ++i) {
        p = put32(p, events[i]);
        p = put32(p, c->handler_code[i][0]);
        p = put32(p, c->handler_code[i][1]);
    }
    for (unsigned i = 0; i < c->instance_count; ++i) { /* functions: a block, how many values it takes and gives */
        const Instance *in = &c->instances[i];
        unsigned taken = 0, given = 0;
        for (unsigned k = 0; k < in->param_count; ++k) taken += cells(c, in->params[k]);
        for (unsigned k = 0; k < in->result_count; ++k) given += cells(c, in->results[k]);
        p = put32(p, in->start);
        p = put32(p, in->count);
        *p++ = (unsigned char)taken, *p++ = (unsigned char)given;
        p = put16(p, 0);
    }
    for (unsigned i = 0; i < c->instance_count; ++i) /* their values' types, those taken then those given */
        for (unsigned k = 0; k < c->instances[i].param_count + c->instances[i].result_count; ++k) {
            const Instance *in = &c->instances[i];
            Kind kind = k < in->param_count ? in->params[k] : in->results[k - in->param_count];
            static const uint8_t types[] = {[NUM] = 0, [TEXT] = 1, [BOOL] = 2, [I64] = 3, [LIST] = 4, [BYTES] = 5};
            if (kind.type != RECORD) {
                *p++ = types[kind.type], *p++ = 0;
                p = put16(p, kind.type == LIST ? (uint32_t)kind.owner : 0);
                continue;
            }
            const Record *r = &c->records[kind.owner];
            for (unsigned f = 0; f < r->fields; ++f) {
                *p++ = types[r->field_kinds[f]], *p++ = 0;
                p = put16(p, r->field_kinds[f] == LIST ? (uint32_t)r->field_record[f] : 0);
            }
        }
    for (unsigned i = 0; i < c->code_count; ++i) {
        *p++ = c->code[i].op; *p++ = 0; *p++ = 0; *p++ = 0;
        p = put32(p, c->code[i].arg);
        p = put64(p, (uint64_t)c->code[i].integer); /* a number's bits, or an i64's */
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
    c->arguments = -1;
    c->fold_list = -1;
    c->code = c->ops;
    c->code_capacity = CODE;
    c->current = -1;
    c->load = load;
    c->load_context = context;
    c->module_count = 1; /* the main source */
    unsigned char *result = NULL;
    if (!setjmp(c->fail)) {
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
    for (unsigned i = 0; i < c->instance_count; ++i) free(c->instances[i].ops);
    free(c->blocks);
    free(c);
    return *module != NULL;
}
