#include "shell.h"

/* ── size formatting ────────────────────────────────────────────────────── */

static char *fmt_size(int64_t bytes)
{
    char *buf = malloc(32);
    if (!buf) return strdup("?");

    double b = (double)bytes;
    if      (bytes < 1024LL)              snprintf(buf, 32, "%lld B",  (long long)bytes);
    else if (bytes < 1024LL * 1024)       snprintf(buf, 32, "%.1f KB", b / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024)snprintf(buf, 32, "%.1f MB", b / (1024.0 * 1024.0));
    else                                  snprintf(buf, 32, "%.1f GB", b / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

/* format a single cell for display (size column gets human-readable format) */
static char *fmt_cell(NshTable *t, int row, int col)
{
    NshVal v = t->rows[row][col];
    if (v.type == VAL_INT && strcmp(t->cols[col], "size") == 0)
        return fmt_size(v.i);
    return val_to_str(v);
}

/* ── table_print ────────────────────────────────────────────────────────── */

void table_print(NshTable *t, FILE *out)
{
    if (!t) return;

    int use_color = isatty(fileno(out));

    /* pre-format all cells and compute column widths */
    int *widths = calloc(t->ncols, sizeof(int));
    if (!widths) return;

    for (int c = 0; c < t->ncols; c++)
        widths[c] = (int)strlen(t->cols[c]);

    char ***cells = NULL;
    if (t->nrows > 0) {
        cells = malloc(t->nrows * sizeof(char **));
        if (!cells) { free(widths); return; }

        for (int r = 0; r < t->nrows; r++) {
            cells[r] = malloc(t->ncols * sizeof(char *));
            if (!cells[r]) { free(widths); return; }
            for (int c = 0; c < t->ncols; c++) {
                cells[r][c] = fmt_cell(t, r, c);
                int w = (int)strlen(cells[r][c]);
                if (w > widths[c]) widths[c] = w;
            }
        }
    }

    /* header */
    if (use_color) fprintf(out, "\033[1;4m");
    for (int c = 0; c < t->ncols; c++) {
        fprintf(out, "%-*s", widths[c], t->cols[c]);
        if (c < t->ncols - 1) fprintf(out, "  ");
    }
    if (use_color) fprintf(out, "\033[0m");
    fputc('\n', out);

    /* separator */
    for (int c = 0; c < t->ncols; c++) {
        for (int i = 0; i < widths[c]; i++) {
            /* U+2500 BOX DRAWINGS LIGHT HORIZONTAL — 3 UTF-8 bytes per char */
            if (use_color) fputs("\xe2\x94\x80", out);
            else           fputc('-', out);
        }
        if (c < t->ncols - 1) fprintf(out, "  ");
    }
    fputc('\n', out);

    /* rows */
    for (int r = 0; r < t->nrows; r++) {
        for (int c = 0; c < t->ncols; c++) {
            /* right-align numbers, left-align strings */
            int is_num = (t->rows[r][c].type == VAL_INT ||
                          t->rows[r][c].type == VAL_FLOAT);
            if (is_num && strcmp(t->cols[c], "size") != 0) {
                /* raw INT not in size col: right-align */
                fprintf(out, "%*s", widths[c], cells[r][c]);
            } else {
                fprintf(out, "%-*s", widths[c], cells[r][c]);
            }
            if (c < t->ncols - 1) fprintf(out, "  ");
            free(cells[r][c]);
        }
        fputc('\n', out);
        free(cells[r]);
    }

    if (t->nrows == 0)
        fprintf(out, "(empty)\n");

    free(cells);
    free(widths);
}

/* ── table_print_json ───────────────────────────────────────────────────── */

/* minimal JSON string escaping */
static void json_str(FILE *out, const char *s)
{
    fputc('"', out);
    for (; s && *s; s++) {
        switch (*s) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n",  out); break;
        case '\r': fputs("\\r",  out); break;
        case '\t': fputs("\\t",  out); break;
        default:   fputc(*s, out);
        }
    }
    fputc('"', out);
}

void table_print_json(NshTable *t, FILE *out)
{
    if (!t) { fputs("[]\n", out); return; }

    fputs("[\n", out);
    for (int r = 0; r < t->nrows; r++) {
        fputs("  {", out);
        for (int c = 0; c < t->ncols; c++) {
            if (c) fputs(", ", out);
            json_str(out, t->cols[c]);
            fputs(": ", out);

            NshVal v = t->rows[r][c];
            switch (v.type) {
            case VAL_NULL:   fputs("null",  out); break;
            case VAL_BOOL:   fputs(v.b ? "true" : "false", out); break;
            case VAL_INT:    fprintf(out, "%lld", (long long)v.i); break;
            case VAL_FLOAT:  fprintf(out, "%g",   v.f); break;
            case VAL_STRING: json_str(out, v.s); break;
            }
        }
        fprintf(out, "}%s\n", (r < t->nrows - 1) ? "," : "");
    }
    fputs("]\n", out);
}

/* ── table_serialize — TSV for piping to external commands ─────────────── */

void table_serialize(NshTable *t, FILE *out)
{
    if (!t) return;

    /* header row */
    for (int c = 0; c < t->ncols; c++) {
        fputs(t->cols[c], out);
        fputc(c < t->ncols - 1 ? '\t' : '\n', out);
    }

    /* data rows — use raw integer for size (not human-readable) */
    for (int r = 0; r < t->nrows; r++) {
        for (int c = 0; c < t->ncols; c++) {
            char *s = val_to_str(t->rows[r][c]);
            fputs(s, out);
            free(s);
            fputc(c < t->ncols - 1 ? '\t' : '\n', out);
        }
    }
}
