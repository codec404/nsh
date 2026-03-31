#include "shell.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <regex.h>

/* ── table builtin registry ─────────────────────────────────────────────── */

static const char *TABLE_BUILTINS[] = {
    /* producers */
    "ls", "env", "history", "sessions",
    /* consumers */
    "where", "select", "sort-by", "first", "last",
    "count", "describe", "get",
    NULL
};

int is_table_builtin(const char *name)
{
    for (int i = 0; TABLE_BUILTINS[i]; i++)
        if (strcmp(name, TABLE_BUILTINS[i]) == 0)
            return 1;
    return 0;
}

/* ── producers ──────────────────────────────────────────────────────────── */

static char *fmt_perm(mode_t m)
{
    char *buf = malloc(11);
    if (!buf) return strdup("?");
    snprintf(buf, 11, "%c%c%c%c%c%c%c%c%c%c",
             S_ISDIR(m) ? 'd' : (S_ISLNK(m) ? 'l' : '-'),
             (m & S_IRUSR) ? 'r' : '-', (m & S_IWUSR) ? 'w' : '-',
             (m & S_IXUSR) ? 'x' : '-',
             (m & S_IRGRP) ? 'r' : '-', (m & S_IWGRP) ? 'w' : '-',
             (m & S_IXGRP) ? 'x' : '-',
             (m & S_IROTH) ? 'r' : '-', (m & S_IWOTH) ? 'w' : '-',
             (m & S_IXOTH) ? 'x' : '-');
    return buf;
}

static NshTable *table_ls(char **argv, int argc)
{
    const char *path = (argc >= 2 && argv[1][0] != '-') ? argv[1] : ".";
    int show_hidden = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0)
            show_hidden = 1;

    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return NULL;
    }

    char *cols[] = { "name", "type", "size", "modified", "permissions" };
    NshTable *t = table_new(cols, 5);
    if (!t) { closedir(dir); return NULL; }

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (!show_hidden && ent->d_name[0] == '.') continue;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

        struct stat st;
        if (lstat(full, &st) < 0) continue;

        const char *type;
        if      (S_ISDIR(st.st_mode))  type = "dir";
        else if (S_ISLNK(st.st_mode))  type = "symlink";
        else if (S_ISREG(st.st_mode))  type = "file";
        else                            type = "other";

        char timebuf[32];
        struct tm *tm_info = localtime(&st.st_mtime);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", tm_info);

        char *perm = fmt_perm(st.st_mode);

        NshVal vals[5] = {
            val_str(ent->d_name),
            val_str(type),
            val_int((int64_t)st.st_size),
            val_str(timebuf),
            val_str_own(perm),
        };
        table_append(t, vals);
        /* val_str_own transferred ownership; free the rest */
        val_free(&vals[0]);
        val_free(&vals[1]);
        val_free(&vals[3]);
    }

    closedir(dir);
    return t;
}

static NshTable *table_env(void)
{
    extern char **environ;
    char *cols[] = { "name", "value" };
    NshTable *t = table_new(cols, 2);
    if (!t) return NULL;

    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq) continue;

        NshVal vals[2] = {
            val_str_own(strndup(*e, (size_t)(eq - *e))),
            val_str(eq + 1),
        };
        table_append(t, vals);
        val_free(&vals[0]);
        val_free(&vals[1]);
    }
    return t;
}

/* ── consumers ──────────────────────────────────────────────────────────── */

/* parse size suffixes: 1kb → 1024, 1.5mb → 1572864 */
static int64_t parse_size_lit(const char *s)
{
    char suffix[8] = {0};
    double v;
    if (sscanf(s, "%lf%7s", &v, suffix) < 1) return (int64_t)atoll(s);

    /* lowercase */
    for (int i = 0; suffix[i]; i++) suffix[i] = (char)tolower((unsigned char)suffix[i]);

    if (strcmp(suffix, "kb") == 0 || strcmp(suffix, "k") == 0) return (int64_t)(v * 1024.0);
    if (strcmp(suffix, "mb") == 0 || strcmp(suffix, "m") == 0) return (int64_t)(v * 1024.0 * 1024.0);
    if (strcmp(suffix, "gb") == 0 || strcmp(suffix, "g") == 0) return (int64_t)(v * 1024.0 * 1024.0 * 1024.0);
    if (strcmp(suffix, "tb") == 0 || strcmp(suffix, "t") == 0) return (int64_t)(v * 1024.0 * 1024.0 * 1024.0 * 1024.0);
    return (int64_t)v;
}

typedef struct { const char *col; const char *op; const char *lit; } Clause;

/*
 * row_matches — evaluate a WHERE clause set against a single row.
 * clause_ops[i] == 0 → && between clause i and i+1; 1 → ||
 */
static int row_matches(NshTable *t, int row, Clause *cl, int ncl, int *ops)
{
    int result = 1;
    for (int i = 0; i < ncl; i++) {
        int cidx = table_col_idx(t, cl[i].col);
        if (cidx < 0) { result = 0; break; }

        NshVal cell = t->rows[row][cidx];
        const char *op  = cl[i].op;
        const char *lit = cl[i].lit;
        int match = 0;

        if (strcmp(op, "=~") == 0 || strcmp(op, "!~") == 0) {
            /* regex match */
            char *s = val_to_str(cell);
            regex_t re;
            if (regcomp(&re, lit, REG_EXTENDED | REG_NOSUB) == 0) {
                match = (regexec(&re, s, 0, NULL, 0) == 0);
                regfree(&re);
            }
            free(s);
            if (strcmp(op, "!~") == 0) match = !match;

        } else if (cell.type == VAL_INT) {
            int64_t lhs = cell.i;
            int64_t rhs = parse_size_lit(lit);
            if      (strcmp(op, "==") == 0) match = (lhs == rhs);
            else if (strcmp(op, "!=") == 0) match = (lhs != rhs);
            else if (strcmp(op, "<")  == 0) match = (lhs <  rhs);
            else if (strcmp(op, ">")  == 0) match = (lhs >  rhs);
            else if (strcmp(op, "<=") == 0) match = (lhs <= rhs);
            else if (strcmp(op, ">=") == 0) match = (lhs >= rhs);

        } else if (cell.type == VAL_FLOAT) {
            double lhs = cell.f, rhs = atof(lit);
            if      (strcmp(op, "==") == 0) match = (lhs == rhs);
            else if (strcmp(op, "!=") == 0) match = (lhs != rhs);
            else if (strcmp(op, "<")  == 0) match = (lhs <  rhs);
            else if (strcmp(op, ">")  == 0) match = (lhs >  rhs);
            else if (strcmp(op, "<=") == 0) match = (lhs <= rhs);
            else if (strcmp(op, ">=") == 0) match = (lhs >= rhs);

        } else {
            /* string comparison */
            char *s = val_to_str(cell);
            int cmp = strcmp(s, lit);
            free(s);
            if      (strcmp(op, "==") == 0) match = (cmp == 0);
            else if (strcmp(op, "!=") == 0) match = (cmp != 0);
            else if (strcmp(op, "<")  == 0) match = (cmp <  0);
            else if (strcmp(op, ">")  == 0) match = (cmp >  0);
            else if (strcmp(op, "<=") == 0) match = (cmp <= 0);
            else if (strcmp(op, ">=") == 0) match = (cmp >= 0);
            else if (strcmp(op, "=~") == 0 || strcmp(op, "!~") == 0) {} /* handled above */
        }

        if (i == 0)              result = match;
        else if (ops[i-1] == 0) result = result && match;  /* && */
        else                    result = result || match;   /* || */
    }
    return result;
}

/*
 * table_where — filter rows matching: col op val [&& col op val ...]
 *
 * argv[0] = "where"
 * argv[1] = col, argv[2] = op, argv[3] = literal
 * argv[4] = "&&"|"||"|"and"|"or"  (optional, repeat)
 */
static NshTable *table_where(NshTable *in, char **argv, int argc)
{
    if (!in) { fprintf(stderr, "where: no input table\n"); return NULL; }
    if (argc < 4) {
        fprintf(stderr, "where: usage: where <col> <op> <value>\n");
        return NULL;
    }

    Clause clauses[16];
    int    clause_ops[16];  /* 0 = &&, 1 = || */
    int    ncl = 0;

    int i = 1;
    while (i + 2 < argc && ncl < 16) {
        clauses[ncl].col = argv[i];
        clauses[ncl].op  = argv[i + 1];
        clauses[ncl].lit = argv[i + 2];
        ncl++;
        i += 3;

        if (i < argc) {
            if (strcmp(argv[i], "&&") == 0 || strcmp(argv[i], "and") == 0)
                clause_ops[ncl - 1] = 0;
            else if (strcmp(argv[i], "||") == 0 || strcmp(argv[i], "or") == 0)
                clause_ops[ncl - 1] = 1;
            else
                break;  /* unknown connector — stop parsing */
            i++;
        }
    }

    if (ncl == 0) {
        fprintf(stderr, "where: could not parse expression\n");
        return NULL;
    }

    /* validate column names */
    for (int c = 0; c < ncl; c++) {
        if (strcmp(clauses[c].op, "=~") != 0 && strcmp(clauses[c].op, "!~") != 0) {
            if (table_col_idx(in, clauses[c].col) < 0) {
                fprintf(stderr, "where: unknown column '%s'\n", clauses[c].col);
                return NULL;
            }
        }
    }

    NshTable *out = table_new(in->cols, in->ncols);
    if (!out) return NULL;

    for (int r = 0; r < in->nrows; r++)
        if (row_matches(in, r, clauses, ncl, clause_ops))
            table_append(out, in->rows[r]);

    return out;
}

static NshTable *table_select(NshTable *in, char **argv, int argc)
{
    if (!in) { fprintf(stderr, "select: no input table\n"); return NULL; }
    if (argc < 2) { fprintf(stderr, "select: expected column names\n"); return NULL; }

    int ncols = argc - 1;
    char **cols = argv + 1;

    /* validate */
    for (int c = 0; c < ncols; c++) {
        if (table_col_idx(in, cols[c]) < 0) {
            fprintf(stderr, "select: unknown column '%s'\n", cols[c]);
            return NULL;
        }
    }

    NshTable *out = table_new(cols, ncols);
    if (!out) return NULL;

    for (int r = 0; r < in->nrows; r++) {
        NshVal vals[MAX_ARGS];
        for (int c = 0; c < ncols; c++)
            vals[c] = in->rows[r][table_col_idx(in, cols[c])];
        table_append(out, vals);
    }
    return out;
}

/* sort helpers — use statics to pass context to qsort callback */
static int g_sort_col  = 0;
static int g_sort_desc = 0;

static int cmp_rows(const void *a, const void *b)
{
    NshVal *ra = *(NshVal **)a;
    NshVal *rb = *(NshVal **)b;
    int cmp = val_compare(ra[g_sort_col], rb[g_sort_col]);
    return g_sort_desc ? -cmp : cmp;
}

static NshTable *table_sort_by(NshTable *in, char **argv, int argc)
{
    if (!in) { fprintf(stderr, "sort-by: no input table\n"); return NULL; }
    if (argc < 2) { fprintf(stderr, "sort-by: expected column name\n"); return NULL; }

    int cidx = table_col_idx(in, argv[1]);
    if (cidx < 0) {
        fprintf(stderr, "sort-by: unknown column '%s'\n", argv[1]);
        return NULL;
    }

    int desc = 0;
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--desc") == 0 || strcmp(argv[i], "--reverse") == 0)
            desc = 1;

    NshTable *out = table_dup(in);
    if (!out) return NULL;

    g_sort_col  = cidx;
    g_sort_desc = desc;
    qsort(out->rows, (size_t)out->nrows, sizeof(NshVal *), cmp_rows);

    return out;
}

static NshTable *table_first(NshTable *in, char **argv, int argc)
{
    if (!in) { fprintf(stderr, "first: no input table\n"); return NULL; }
    int n = (argc >= 2) ? atoi(argv[1]) : 5;
    if (n < 0) n = 0;
    NshTable *out = table_new(in->cols, in->ncols);
    for (int r = 0; r < in->nrows && r < n; r++)
        table_append(out, in->rows[r]);
    return out;
}

static NshTable *table_last(NshTable *in, char **argv, int argc)
{
    if (!in) { fprintf(stderr, "last: no input table\n"); return NULL; }
    int n = (argc >= 2) ? atoi(argv[1]) : 5;
    if (n < 0) n = 0;
    int start = in->nrows - n;
    if (start < 0) start = 0;
    NshTable *out = table_new(in->cols, in->ncols);
    for (int r = start; r < in->nrows; r++)
        table_append(out, in->rows[r]);
    return out;
}

static NshTable *table_count(NshTable *in)
{
    if (!in) { fprintf(stderr, "count: no input table\n"); return NULL; }
    char *cols[] = { "count" };
    NshTable *out = table_new(cols, 1);
    NshVal v = val_int((int64_t)in->nrows);
    table_append(out, &v);
    return out;
}

static NshTable *table_describe(NshTable *in)
{
    if (!in) { fprintf(stderr, "describe: no input table\n"); return NULL; }
    char *cols[] = { "column", "type", "count" };
    NshTable *out = table_new(cols, 3);

    for (int c = 0; c < in->ncols; c++) {
        /* infer type from first non-null value */
        const char *type_str = "null";
        for (int r = 0; r < in->nrows; r++) {
            switch (in->rows[r][c].type) {
            case VAL_INT:    type_str = "int";    goto found;
            case VAL_FLOAT:  type_str = "float";  goto found;
            case VAL_STRING: type_str = "string"; goto found;
            case VAL_BOOL:   type_str = "bool";   goto found;
            default: break;
            }
        }
        found:;

        NshVal vals[3] = {
            val_str(in->cols[c]),
            val_str(type_str),
            val_int((int64_t)in->nrows),
        };
        table_append(out, vals);
        for (int i = 0; i < 3; i++) val_free(&vals[i]);
    }
    return out;
}

static NshTable *table_get(NshTable *in, char **argv, int argc)
{
    if (!in) { fprintf(stderr, "get: no input table\n"); return NULL; }
    if (argc < 2) { fprintf(stderr, "get: expected column name\n"); return NULL; }

    int cidx = table_col_idx(in, argv[1]);
    if (cidx < 0) {
        fprintf(stderr, "get: unknown column '%s'\n", argv[1]);
        return NULL;
    }

    char *cols[] = { argv[1] };
    NshTable *out = table_new(cols, 1);
    for (int r = 0; r < in->nrows; r++)
        table_append(out, &in->rows[r][cidx]);
    return out;
}

/* ── dispatcher ─────────────────────────────────────────────────────────── */

/*
 * run_table_cmd — run one table command.
 *
 * `input` is the upstream table (NULL for producers like ls/env).
 * Returns a new NshTable* (caller must free), or NULL on error.
 * Does NOT free `input` — the caller manages that.
 */
NshTable *run_table_cmd(char **argv, int argc, NshTable *input)
{
    if (!argv || argc == 0) return NULL;
    const char *cmd = argv[0];

    if (strcmp(cmd, "ls")       == 0) return table_ls(argv, argc);
    if (strcmp(cmd, "env")      == 0) return table_env();
    if (strcmp(cmd, "history")  == 0) return hist_query(argv, argc);
    if (strcmp(cmd, "sessions") == 0) return hist_sessions();
    if (strcmp(cmd, "where")    == 0) return table_where(input, argv, argc);
    if (strcmp(cmd, "select")   == 0) return table_select(input, argv, argc);
    if (strcmp(cmd, "sort-by")  == 0) return table_sort_by(input, argv, argc);
    if (strcmp(cmd, "first")    == 0) return table_first(input, argv, argc);
    if (strcmp(cmd, "last")     == 0) return table_last(input, argv, argc);
    if (strcmp(cmd, "count")    == 0) return table_count(input);
    if (strcmp(cmd, "describe") == 0) return table_describe(input);
    if (strcmp(cmd, "get")      == 0) return table_get(input, argv, argc);

    fprintf(stderr, "nsh: unknown table command '%s'\n", cmd);
    return NULL;
}
