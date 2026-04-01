#include "shell.h"

/* ── dynamic string builder ─────────────────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} Str;

static void str_init(Str *s)
{
    s->cap = 64;
    s->len = 0;
    s->buf = malloc(s->cap);
    if (s->buf) s->buf[0] = '\0';
}

static void str_reserve(Str *s, size_t extra)
{
    if (!s->buf || s->len + extra + 1 > s->cap) {
        s->cap = (s->len + extra + 1) * 2 + 64;
        char *nb = realloc(s->buf, s->cap);
        if (!nb) { perror("realloc"); return; }
        s->buf = nb;
    }
}

static void str_push(Str *s, char c)
{
    str_reserve(s, 1);
    if (s->buf) { s->buf[s->len++] = c; s->buf[s->len] = '\0'; }
}

static void str_cat(Str *s, const char *t, size_t n)
{
    if (!t || n == 0) return;
    str_reserve(s, n);
    if (s->buf) { memcpy(s->buf + s->len, t, n); s->len += n; s->buf[s->len] = '\0'; }
}

/* Return the owned buffer (caller must free). Reset s to empty. */
static char *str_take(Str *s)
{
    if (!s->buf) return strdup("");
    s->buf[s->len] = '\0';
    char *r = s->buf;
    s->buf = NULL;
    s->len = s->cap = 0;
    return r;
}

static void str_free(Str *s)
{
    free(s->buf);
    s->buf = NULL;
    s->len = s->cap = 0;
}

/* ── variable expansion: *pp points to char AFTER '$' ──────────────────── */

static void expand_dollar(Str *out, const char **pp);   /* forward decl */

/*
 * do_cmdsub — fork a subshell, run cmd_text with stdout on a pipe,
 * collect the output into `out`, strip trailing newlines (POSIX).
 */
static void do_cmdsub(Str *out, const char *cmd_text)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe (cmdsub)"); return; }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork (cmdsub)");
        close(pipefd[0]); close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        /* child: stdout → pipe write end */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(1);
        close(pipefd[1]);

        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);

        /* evaluate the command string through the full shell pipeline */
        char *mutable = strdup(cmd_text);
        if (!mutable) _exit(1);

        int nt = 0;
        char **toks = tokenize_raw(mutable, &nt);
        free(mutable);

        if (toks && nt > 0) {
            AstNode *ast = parse_program(toks, nt);
            free_tokens(toks);
            if (ast) {
                int st = execute_ast(ast, &g_ctx);
                free_ast(ast);
                _exit(st);
            }
        }
        free_tokens(toks);
        _exit(1);
    }

    /* parent: read child's stdout */
    close(pipefd[1]);

    Str result;
    str_init(&result);
    char buf[512];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        str_cat(&result, buf, (size_t)n);
    close(pipefd[0]);

    waitpid(pid, NULL, 0);

    /* strip trailing newlines (POSIX §2.6.3) */
    while (result.len > 0 && result.buf[result.len - 1] == '\n')
        result.len--;
    if (result.buf) result.buf[result.len] = '\0';

    if (result.buf && result.len > 0)
        str_cat(out, result.buf, result.len);

    str_free(&result);
}

static void expand_dollar(Str *out, const char **pp)
{
    const char *p = *pp;

    if (*p == '?') {
        /* $? — last exit status */
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", last_status);
        str_cat(out, buf, strlen(buf));
        p++;

    } else if (*p == '$') {
        /* $$ — shell PID */
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)getpid());
        str_cat(out, buf, strlen(buf));
        p++;

    } else if (*p == '!') {
        /* $! — PID of last background job's process group */
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)last_bg_pgid);
        str_cat(out, buf, strlen(buf));
        p++;

    } else if (*p == '(') {
        /*
         * $(cmd) — command substitution.
         *
         * Collect everything up to the matching ')' into a buffer,
         * skipping over quoted strings so a ')' inside quotes doesn't
         * close the substitution prematurely.
         */
        p++;    /* skip '(' */
        Str cmd;
        str_init(&cmd);
        int depth = 1;

        while (*p && depth > 0) {
            if (*p == '\'') {
                /* single-quoted span: copy verbatim */
                str_push(&cmd, *p++);
                while (*p && *p != '\'') str_push(&cmd, *p++);
                if (*p) str_push(&cmd, *p++);

            } else if (*p == '"') {
                /* double-quoted span: copy, honour backslash */
                str_push(&cmd, *p++);
                while (*p && *p != '"') {
                    if (*p == '\\' && *(p + 1)) str_push(&cmd, *p++);
                    str_push(&cmd, *p++);
                }
                if (*p) str_push(&cmd, *p++);

            } else if (*p == '(') {
                depth++;
                str_push(&cmd, *p++);

            } else if (*p == ')') {
                depth--;
                if (depth > 0) str_push(&cmd, *p++);
                else           p++;     /* skip closing ')' */

            } else {
                str_push(&cmd, *p++);
            }
        }

        char *cmd_text = str_take(&cmd);
        do_cmdsub(out, cmd_text);
        free(cmd_text);

    } else if (*p == '{') {
        /* ${VAR} */
        p++;    /* skip '{' */
        const char *start = p;
        while (*p && *p != '}') p++;
        char *name = strndup(start, (size_t)(p - start));
        const char *val = name ? getenv(name) : NULL;
        if (val) str_cat(out, val, strlen(val));
        free(name);
        if (*p == '}') p++;

    } else if (isalpha((unsigned char)*p) || *p == '_') {
        /* $VAR */
        const char *start = p;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        char *name = strndup(start, (size_t)(p - start));
        const char *val = name ? getenv(name) : NULL;
        if (val) str_cat(out, val, strlen(val));
        free(name);

    } else {
        /* bare '$' with nothing recognisable after it — emit literally */
        str_push(out, '$');
        /* do NOT advance p — caller will process *p normally */
    }

    *pp = p;
}

/* ── operator helpers ───────────────────────────────────────────────────── */

static int is_op_char(char c)
{
    return c == '|' || c == '>' || c == '<' || c == '&'
        || c == '{' || c == '}' || c == ';';
}

/*
 * read_op — consume one operator token from *pp and return a strdup'd string.
 * Recognises ">>" as a two-character token; everything else is one character.
 */
static char *read_op(const char **pp)
{
    const char *p = *pp;
    char tok[3] = { 0, 0, 0 };

    if (*p == '>' && *(p + 1) == '>') {
        tok[0] = tok[1] = '>'; *pp = p + 2;
    } else {
        tok[0] = *p; *pp = p + 1;
    }
    return strdup(tok);
}

/* ── main tokenizer ─────────────────────────────────────────────────────── */

/*
 * tokenize — scan `line` and produce a NULL-terminated array of tokens.
 *
 * Handles:
 *   'single quotes'   — no expansion, no word splitting inside
 *   "double quotes"   — $VAR, $(cmd) expanded; no word splitting
 *   \c                — backslash escape (unquoted and double-quoted)
 *   $VAR / ${VAR}     — environment variable substitution
 *   $? $$ $!          — special parameter expansion
 *   $(cmd)            — command substitution via fork+pipe
 *   |  >  >>  <  &    — operator tokens; also terminate adjacent words
 *
 * Operators are recognised even without surrounding whitespace, so
 *   echo foo>bar  tokenises as: echo  foo  >  bar
 */
char **tokenize(char *line, int *out_ntokens)
{
    char **tokens = calloc(MAX_ARGS + 1, sizeof(char *));
    if (!tokens) { perror("calloc"); return NULL; }
    int n = 0;
    const char *p = line;

    while (*p) {
        /* skip horizontal whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        /* newline → statement separator (like ;) */
        if (*p == '\n') {
            if (n < MAX_ARGS) tokens[n++] = strdup(";");
            p++;
            continue;
        }

        /* # comment → skip to end of line */
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }

        if (n >= MAX_ARGS) {
            fprintf(stderr, "nsh: too many tokens (max %d)\n", MAX_ARGS);
            break;
        }

        /* ── operator token ── */
        if (is_op_char(*p)) {
            tokens[n++] = read_op(&p);
            continue;
        }

        /* ── word token ── */
        Str word;
        str_init(&word);

        /*
         * Tilde expansion: an unquoted '~' at the START of a word,
         * followed by '/' or end-of-token, expands to $HOME.
         * Examples:  ~  ~/foo  →  /Users/x  /Users/x/foo
         * Not expanded: "~"  '~'  foo~bar  ~foo (user home dirs — not implemented)
         */
        if (*p == '~' && (*(p+1) == '/' || *(p+1) == '\0' ||
                           *(p+1) == ' ' || *(p+1) == '\t' || is_op_char(*(p+1)))) {
            const char *home = getenv("HOME");
            if (home) str_cat(&word, home, strlen(home));
            else      str_push(&word, '~');   /* HOME unset: keep literal */
            p++;
        }

        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && !is_op_char(*p)) {
            if (*p == '\'') {
                /*
                 * Single quotes: everything literal until the matching '.
                 * Not even backslash is special inside single quotes.
                 */
                p++;    /* skip opening ' */
                while (*p && *p != '\'') str_push(&word, *p++);
                if (*p) p++;    /* skip closing ' */

            } else if (*p == '"') {
                /*
                 * Double quotes: $-expansion active, backslash escapes
                 * $  "  \  and newline.  Everything else is literal.
                 */
                p++;    /* skip opening " */
                while (*p && *p != '"') {
                    if (*p == '\\') {
                        char next = *(p + 1);
                        if (next == '$' || next == '"' || next == '\\') {
                            p++;    /* skip backslash */
                            str_push(&word, *p++);
                        } else if (next == '\n') {
                            p += 2; /* line continuation */
                        } else {
                            /* backslash is literal in double quotes otherwise */
                            str_push(&word, *p++);
                        }
                    } else if (*p == '$') {
                        p++;
                        expand_dollar(&word, &p);
                    } else {
                        str_push(&word, *p++);
                    }
                }
                if (*p) p++;    /* skip closing " */

            } else if (*p == '\\') {
                /*
                 * Unquoted backslash: next character is literal,
                 * \<newline> is a line continuation (eaten).
                 */
                p++;
                if (*p == '\n') {
                    p++;    /* line continuation */
                } else if (*p) {
                    str_push(&word, *p++);
                }

            } else if (*p == '$') {
                p++;
                expand_dollar(&word, &p);

            } else {
                str_push(&word, *p++);
            }
        }

        /*
         * Emit the word even if its buffer is empty — adjacent empty quotes
         * like '' or "" must produce an empty-string argument.
         */
        tokens[n++] = str_take(&word);
    }

    tokens[n] = NULL;
    if (out_ntokens) *out_ntokens = n;
    return tokens;
}

void free_tokens(char **tokens)
{
    if (!tokens) return;
    for (int i = 0; tokens[i]; i++)
        free(tokens[i]);
    free(tokens);
}

/* ── raw (deferred-expansion) tokenizer ─────────────────────────────────── */

/*
 * Store a $... expression as a literal string — no environment lookup.
 * Used by tokenize_raw so that $VAR, ${VAR}, $(), $?, etc. are kept
 * verbatim in the token and expanded at execution time by expand_word().
 */
static void raw_dollar_lit(Str *out, const char **pp)
{
    const char *p = *pp;
    str_push(out, '$');

    if (*p == '(') {
        str_push(out, '('); p++;
        int depth = 1;
        while (*p && depth > 0) {
            if (*p == '(') depth++;
            else if (*p == ')') {
                depth--;
                if (!depth) { str_push(out, ')'); p++; break; }
            }
            str_push(out, *p++);
        }
    } else if (*p == '{') {
        str_push(out, '{'); p++;
        while (*p && *p != '}') str_push(out, *p++);
        if (*p == '}') { str_push(out, '}'); p++; }
    } else if (*p == '?' || *p == '$' || *p == '!' || *p == '#') {
        str_push(out, *p++);
    } else if (isalpha((unsigned char)*p) || *p == '_') {
        while (isalnum((unsigned char)*p) || *p == '_') str_push(out, *p++);
    }
    /* bare $ with nothing recognisable → already pushed the '$' */
    *pp = p;
}

/*
 * tokenize_raw — identical to tokenize but defers $-expansion and ~ expansion.
 * The tokens it produces are suitable for embedding in the AST; they will be
 * fully expanded at execution time by expand_word() / expand_argv().
 */
char **tokenize_raw(char *line, int *out_ntokens)
{
    char **tokens = calloc(MAX_ARGS + 1, sizeof(char *));
    if (!tokens) { perror("calloc"); return NULL; }
    int n = 0;
    const char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        if (*p == '\n') {
            if (n < MAX_ARGS) tokens[n++] = strdup(";");
            p++;
            continue;
        }
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }

        if (n >= MAX_ARGS) {
            fprintf(stderr, "nsh: too many tokens (max %d)\n", MAX_ARGS);
            break;
        }

        if (is_op_char(*p)) { tokens[n++] = read_op(&p); continue; }

        Str word;
        str_init(&word);

        /* tilde kept literal — expanded at execution time by expand_word */
        if (*p == '~' && (*(p+1) == '/' || *(p+1) == '\0' ||
                           *(p+1) == ' ' || *(p+1) == '\t' || is_op_char(*(p+1)))) {
            str_push(&word, '~');
            p++;
        }

        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && !is_op_char(*p)) {
            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') str_push(&word, *p++);
                if (*p) p++;

            } else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\') {
                        char next = *(p + 1);
                        if (next == '$' || next == '"' || next == '\\') {
                            p++;
                            str_push(&word, *p++);
                        } else if (next == '\n') {
                            p += 2;
                        } else {
                            str_push(&word, *p++);
                        }
                    } else if (*p == '$') {
                        p++;
                        raw_dollar_lit(&word, &p);   /* keep literal */
                    } else {
                        str_push(&word, *p++);
                    }
                }
                if (*p) p++;

            } else if (*p == '\\') {
                p++;
                if (*p == '\n') p++;
                else if (*p) str_push(&word, *p++);

            } else if (*p == '$') {
                p++;
                raw_dollar_lit(&word, &p);   /* keep literal */

            } else {
                str_push(&word, *p++);
            }
        }

        tokens[n++] = str_take(&word);
    }

    tokens[n] = NULL;
    if (out_ntokens) *out_ntokens = n;
    return tokens;
}

/* ── word/argv expansion ─────────────────────────────────────────────────── */

/*
 * expand_word — expand $VAR, ${VAR}, $(), ~/ etc. in a single raw token
 * against the current process environment.  Called at execution time.
 */
char *expand_word(const char *raw)
{
    if (!raw) return strdup("");

    Str out;
    str_init(&out);
    const char *p = raw;

    /* tilde at word start */
    if (*p == '~' && (*(p+1) == '/' || *(p+1) == '\0')) {
        const char *home = getenv("HOME");
        if (home) str_cat(&out, home, strlen(home));
        else      str_push(&out, '~');
        p++;
    }

    while (*p) {
        if (*p == '$') {
            p++;
            expand_dollar(&out, &p);
        } else {
            str_push(&out, *p++);
        }
    }
    return str_take(&out);
}

/*
 * expand_argv — expand an array of n raw tokens into a fresh heap-allocated
 * NULL-terminated array.  Caller frees with free_tokens().
 */
char **expand_argv(char **raw, int n)
{
    char **out = calloc(n + 1, sizeof(char *));
    if (!out) return NULL;
    for (int i = 0; i < n; i++)
        out[i] = expand_word(raw[i]);
    out[n] = NULL;
    return out;
}
