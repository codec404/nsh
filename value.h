#ifndef VALUE_H
#define VALUE_H

#include <stdint.h>
#include <stdio.h>

/* ── tagged value type ──────────────────────────────────────────────────── */

typedef enum {
    VAL_NULL   = 0,
    VAL_BOOL,
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
} ValType;

typedef struct {
    ValType type;
    union {
        int      b;     /* VAL_BOOL   */
        int64_t  i;     /* VAL_INT    */
        double   f;     /* VAL_FLOAT  */
        char    *s;     /* VAL_STRING — owned */
    };
} NshVal;

/*
 * NshTable — a typed, columnar table.
 *
 * cols[c]     — column name, owned by the table
 * rows[r][c]  — NshVal at row r, column c; owned by the table
 *
 * All public functions that add rows copy the NshVal array passed in,
 * so callers do not transfer ownership.
 */
typedef struct {
    char   **cols;
    int      ncols;
    NshVal **rows;
    int      nrows;
    int      cap;
} NshTable;

/* ── NshVal constructors ────────────────────────────────────────────────── */
NshVal   val_null(void);
NshVal   val_bool(int b);
NshVal   val_int(int64_t i);
NshVal   val_float(double f);
NshVal   val_str(const char *s);    /* strdup's s */
NshVal   val_str_own(char *s);      /* takes ownership of s */

/* ── NshVal operations ──────────────────────────────────────────────────── */
void     val_free(NshVal *v);
NshVal   val_dup(NshVal v);
int      val_compare(NshVal a, NshVal b);   /* <0, 0, >0 */
char    *val_to_str(NshVal v);              /* caller must free() */
int64_t  val_to_int(NshVal v);
double   val_to_float(NshVal v);

/* ── NshTable lifecycle ─────────────────────────────────────────────────── */
NshTable *table_new(char **cols, int ncols);        /* copies cols */
void      table_append(NshTable *t, NshVal *vals);  /* copies vals (ncols of them) */
NshTable *table_dup(NshTable *t);
void      table_free(NshTable *t);

/* ── NshTable accessors ─────────────────────────────────────────────────── */
int       table_col_idx(NshTable *t, const char *name);   /* -1 if not found */

/* ── rendering & serialization ──────────────────────────────────────────── */
void table_print(NshTable *t, FILE *out);           /* aligned columns, colors on TTY */
void table_print_json(NshTable *t, FILE *out);      /* JSON array of objects */
void table_serialize(NshTable *t, FILE *out);       /* TSV for external command stdin */

/* ── table builtin dispatch ─────────────────────────────────────────────── */
int       is_table_builtin(const char *name);
NshTable *run_table_cmd(char **argv, int argc, NshTable *input);

#endif
