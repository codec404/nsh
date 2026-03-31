#include "shell.h"

/* ── NshVal constructors ────────────────────────────────────────────────── */

NshVal val_null(void)         { NshVal v; v.type = VAL_NULL;  v.i = 0;       return v; }
NshVal val_bool(int b)        { NshVal v; v.type = VAL_BOOL;  v.b = !!b;     return v; }
NshVal val_int(int64_t i)     { NshVal v; v.type = VAL_INT;   v.i = i;       return v; }
NshVal val_float(double f)    { NshVal v; v.type = VAL_FLOAT; v.f = f;       return v; }
NshVal val_str_own(char *s)   { NshVal v; v.type = VAL_STRING; v.s = s;      return v; }
NshVal val_str(const char *s) { return val_str_own(s ? strdup(s) : strdup("")); }

/* ── NshVal operations ──────────────────────────────────────────────────── */

void val_free(NshVal *v)
{
    if (v->type == VAL_STRING) { free(v->s); v->s = NULL; }
    v->type = VAL_NULL;
}

NshVal val_dup(NshVal v)
{
    if (v.type == VAL_STRING)
        return val_str(v.s);
    return v;
}

/* human-readable string — caller must free() */
char *val_to_str(NshVal v)
{
    char *buf;
    switch (v.type) {
    case VAL_NULL:   return strdup("");
    case VAL_BOOL:   return strdup(v.b ? "true" : "false");
    case VAL_INT:
        buf = malloc(32);
        snprintf(buf, 32, "%lld", (long long)v.i);
        return buf;
    case VAL_FLOAT:
        buf = malloc(32);
        snprintf(buf, 32, "%g", v.f);
        return buf;
    case VAL_STRING: return strdup(v.s ? v.s : "");
    }
    return strdup("");
}

int64_t val_to_int(NshVal v)
{
    switch (v.type) {
    case VAL_INT:    return v.i;
    case VAL_FLOAT:  return (int64_t)v.f;
    case VAL_BOOL:   return (int64_t)v.b;
    case VAL_STRING: return v.s ? atoll(v.s) : 0;
    default:         return 0;
    }
}

double val_to_float(NshVal v)
{
    switch (v.type) {
    case VAL_INT:    return (double)v.i;
    case VAL_FLOAT:  return v.f;
    case VAL_BOOL:   return (double)v.b;
    case VAL_STRING: return v.s ? atof(v.s) : 0.0;
    default:         return 0.0;
    }
}

int val_compare(NshVal a, NshVal b)
{
    /* numeric comparison when both sides are numeric */
    if ((a.type == VAL_INT || a.type == VAL_FLOAT) &&
        (b.type == VAL_INT || b.type == VAL_FLOAT)) {
        double da = val_to_float(a), db = val_to_float(b);
        return (da > db) - (da < db);
    }
    /* string fallback */
    char *sa = val_to_str(a), *sb = val_to_str(b);
    int cmp = strcmp(sa, sb);
    free(sa); free(sb);
    return cmp;
}

/* ── NshTable lifecycle ─────────────────────────────────────────────────── */

NshTable *table_new(char **cols, int ncols)
{
    NshTable *t = calloc(1, sizeof(NshTable));
    if (!t) { perror("calloc"); return NULL; }

    t->ncols = ncols;
    t->cols  = malloc(ncols * sizeof(char *));
    if (!t->cols) { perror("malloc"); free(t); return NULL; }

    for (int i = 0; i < ncols; i++)
        t->cols[i] = strdup(cols[i]);

    t->rows = NULL;
    t->nrows = 0;
    t->cap   = 0;
    return t;
}

void table_append(NshTable *t, NshVal *vals)
{
    if (t->nrows >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        NshVal **nr = realloc(t->rows, t->cap * sizeof(NshVal *));
        if (!nr) { perror("realloc"); return; }
        t->rows = nr;
    }

    NshVal *row = malloc(t->ncols * sizeof(NshVal));
    if (!row) { perror("malloc"); return; }

    for (int i = 0; i < t->ncols; i++)
        row[i] = val_dup(vals[i]);

    t->rows[t->nrows++] = row;
}

NshTable *table_dup(NshTable *t)
{
    NshTable *out = table_new(t->cols, t->ncols);
    if (!out) return NULL;
    for (int r = 0; r < t->nrows; r++)
        table_append(out, t->rows[r]);
    return out;
}

void table_free(NshTable *t)
{
    if (!t) return;
    for (int c = 0; c < t->ncols; c++) free(t->cols[c]);
    free(t->cols);
    for (int r = 0; r < t->nrows; r++) {
        for (int c = 0; c < t->ncols; c++) val_free(&t->rows[r][c]);
        free(t->rows[r]);
    }
    free(t->rows);
    free(t);
}

/* ── NshTable accessors ─────────────────────────────────────────────────── */

int table_col_idx(NshTable *t, const char *name)
{
    for (int c = 0; c < t->ncols; c++)
        if (strcmp(t->cols[c], name) == 0)
            return c;
    return -1;
}
