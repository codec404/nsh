#include "shell.h"

/*
 * parse_cmd — fill one Cmd from a token slice.
 *
 * table_cmd == 1: this is a table builtin (where, sort-by, etc.).
 *   In that mode, >, < are comparison operators that belong in argv —
 *   NOT I/O redirections.  >> is still treated as append-redirect because
 *   no table operator uses it.  Output redirection is handled later by
 *   execute_table_pipeline (it inspects redir_out after the table runs).
 *
 * table_cmd == 0: classic external-command parsing, redirects consumed here.
 */
static int parse_cmd(Cmd *cmd, char **tokens, int ntokens, int table_cmd)
{
    cmd->argv       = calloc(ntokens + 1, sizeof(char *));
    cmd->argc       = 0;
    cmd->redir_in   = NULL;
    cmd->redir_out  = NULL;
    cmd->append_out = 0;

    if (!cmd->argv) { perror("calloc"); return -1; }

    for (int i = 0; i < ntokens; i++) {
        if (!table_cmd && strcmp(tokens[i], "<") == 0) {
            if (i + 1 >= ntokens) {
                fprintf(stderr, "nsh: syntax error: expected filename after '<'\n");
                return -1;
            }
            free(cmd->redir_in);
            cmd->redir_in = strdup(tokens[++i]);

        } else if (strcmp(tokens[i], ">>") == 0) {
            if (i + 1 >= ntokens) {
                fprintf(stderr, "nsh: syntax error: expected filename after '>>'\n");
                return -1;
            }
            free(cmd->redir_out);
            cmd->redir_out  = strdup(tokens[++i]);
            cmd->append_out = 1;

        } else if (!table_cmd && strcmp(tokens[i], ">") == 0) {
            if (i + 1 >= ntokens) {
                fprintf(stderr, "nsh: syntax error: expected filename after '>'\n");
                return -1;
            }
            free(cmd->redir_out);
            cmd->redir_out  = strdup(tokens[++i]);
            cmd->append_out = 0;

        } else {
            cmd->argv[cmd->argc++] = strdup(tokens[i]);
        }
    }
    cmd->argv[cmd->argc] = NULL;

    if (cmd->argc == 0) {
        fprintf(stderr, "nsh: syntax error: empty command in pipeline\n");
        return -1;
    }
    return 0;
}

/*
 * parse_pipeline — turn a flat token array into a Pipeline.
 *
 * Detects a trailing "&" token and sets pipeline->background = 1, then
 * excludes it before dispatching to parse_cmd().
 */
Pipeline *parse_pipeline(char **tokens, int ntokens, const char *cmdline)
{
    int background = 0;

    /* strip trailing & */
    if (ntokens > 0 && strcmp(tokens[ntokens - 1], "&") == 0) {
        background = 1;
        ntokens--;          /* hide it from the rest of the parser */
    }

    if (ntokens == 0) {
        fprintf(stderr, "nsh: syntax error: no command before '&'\n");
        return NULL;
    }

    int ncmds = 1;
    for (int i = 0; i < ntokens; i++)
        if (strcmp(tokens[i], "|") == 0)
            ncmds++;

    Pipeline *p = malloc(sizeof(Pipeline));
    if (!p) { perror("malloc"); return NULL; }

    p->cmds       = calloc(ncmds, sizeof(Cmd));
    p->ncmds      = ncmds;
    p->background = background;
    p->cmdline    = cmdline ? strdup(cmdline) : NULL;

    if (!p->cmds) { perror("calloc"); free(p); return NULL; }

    /*
     * Pre-scan to identify which segment indices are table builtins.
     * We need this before parsing so parse_cmd knows whether to treat
     * > and < as operators or redirections.
     *
     * Strategy: collect the first token of each segment, then check
     * is_table_builtin().  The last table segment in a run is allowed
     * to use > as a redirect (so `ls > out.txt` still works).  But
     * intermediate table segments treat > as a comparison operator.
     */

    /* first pass: identify segment boundaries and first tokens */
    char *first_tok[MAX_ARGS];
    int   seg_is_table[MAX_ARGS];
    {
        int sidx = 0, in_seg = 0;
        for (int i = 0; i <= ntokens && sidx < MAX_ARGS; i++) {
            if (i == ntokens || strcmp(tokens[i], "|") == 0) {
                if (in_seg) sidx++;
                in_seg = 0;
            } else if (!in_seg) {
                first_tok[sidx] = tokens[i];
                in_seg = 1;
            }
        }
        for (int s = 0; s < ncmds; s++)
            seg_is_table[s] = is_table_builtin(first_tok[s]);
    }

    int cmd_idx   = 0;
    int seg_start = 0;

    for (int i = 0; i <= ntokens; i++) {
        if (i == ntokens || strcmp(tokens[i], "|") == 0) {
            int seg_len = i - seg_start;

            if (seg_len == 0) {
                fprintf(stderr, "nsh: syntax error: empty pipeline segment\n");
                free_pipeline(p);
                return NULL;
            }

            /*
             * Table segments ALWAYS treat > and < as comparison operators,
             * never as I/O redirections.  To redirect a table pipeline's
             * output, use >> (append) or wrap in a subshell.  The executor
             * reads redir_out on the last table Cmd to handle >> redirects.
             */
            int is_table = seg_is_table[cmd_idx];

            if (parse_cmd(&p->cmds[cmd_idx], tokens + seg_start, seg_len, is_table) != 0) {
                free_pipeline(p);
                return NULL;
            }

            cmd_idx++;
            seg_start = i + 1;
        }
    }

    return p;
}

void free_pipeline(Pipeline *p)
{
    if (!p) return;
    for (int i = 0; i < p->ncmds; i++) {
        Cmd *c = &p->cmds[i];
        for (int j = 0; j < c->argc; j++)
            free(c->argv[j]);
        free(c->argv);
        free(c->redir_in);
        free(c->redir_out);
    }
    free(p->cmds);
    free(p->cmdline);
    free(p);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Recursive-descent AST parser
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── token stream cursor ─────────────────────────────────────────────────── */

typedef struct { char **toks; int n, pos; } TS;

static const char *ts_peek(TS *ts)
{
    if (ts->pos >= ts->n) return NULL;
    return ts->toks[ts->pos];
}

static const char *ts_next(TS *ts)
{
    if (ts->pos >= ts->n) return NULL;
    return ts->toks[ts->pos++];
}

static int ts_eat(TS *ts, const char *tok)
{
    if (ts->pos < ts->n && strcmp(ts->toks[ts->pos], tok) == 0)
        { ts->pos++; return 1; }
    return 0;
}

/* skip consecutive semicolons / newlines */
static void ts_skip_semi(TS *ts)
{
    while (ts->pos < ts->n && strcmp(ts->toks[ts->pos], ";") == 0)
        ts->pos++;
}

/* ── forward declarations ────────────────────────────────────────────────── */
static AstNode *parse_stmt(TS *ts);

/* ── helpers ─────────────────────────────────────────────────────────────── */

static AstNode *make_node(AstKind k)
{
    AstNode *n = calloc(1, sizeof(AstNode));
    if (n) n->kind = k;
    return n;
}

/*
 * Collect raw tokens from ts into an AST_PIPELINE node, stopping before
 * structural tokens: {  }  ;  else
 * Tokens are stored unexpanded; execute_ast() will expand + execute them.
 */
static AstNode *parse_pipeline_ts(TS *ts)
{
    char *buf[MAX_ARGS];
    int   n = 0;

    while (ts->pos < ts->n) {
        const char *t = ts->toks[ts->pos];
        if (strcmp(t, "{")    == 0 || strcmp(t, "}") == 0 ||
            strcmp(t, ";")    == 0 || strcmp(t, "else") == 0)
            break;
        buf[n++] = ts->toks[ts->pos++];
        if (n >= MAX_ARGS) break;
    }

    if (n == 0) return NULL;

    AstNode *node = make_node(AST_PIPELINE);
    if (!node) return NULL;
    node->u.pipeline.raw = malloc(n * sizeof(char *));
    node->u.pipeline.n   = n;
    for (int i = 0; i < n; i++)
        node->u.pipeline.raw[i] = strdup(buf[i]);
    return node;
}

/* ── block: { stmt; stmt; ... } ──────────────────────────────────────────── */

static AstNode *parse_block(TS *ts)
{
    if (!ts_eat(ts, "{")) {
        fprintf(stderr, "nsh: syntax error: expected '{'\n");
        return NULL;
    }

    AstNode **stmts = NULL;
    int nstmts = 0, cap = 0;

    for (;;) {
        ts_skip_semi(ts);
        const char *peek = ts_peek(ts);
        if (!peek || strcmp(peek, "}") == 0) break;

        AstNode *s = parse_stmt(ts);
        if (!s) break;

        if (nstmts >= cap) {
            cap = cap ? cap * 2 : 4;
            stmts = realloc(stmts, cap * sizeof(AstNode *));
        }
        stmts[nstmts++] = s;
    }

    if (!ts_eat(ts, "}")) {
        fprintf(stderr, "nsh: syntax error: expected '}'\n");
        for (int i = 0; i < nstmts; i++) free_ast(stmts[i]);
        free(stmts);
        return NULL;
    }

    AstNode *node = make_node(AST_BLOCK);
    if (!node) { for (int i=0;i<nstmts;i++) free_ast(stmts[i]); free(stmts); return NULL; }
    node->u.block.stmts  = stmts;
    node->u.block.nstmts = nstmts;
    return node;
}

/* ── if ──────────────────────────────────────────────────────────────────── */

static AstNode *parse_if(TS *ts)
{
    ts_next(ts);  /* consume "if" */

    /* condition: tokens until { */
    char *buf[MAX_ARGS]; int n = 0;
    while (ts->pos < ts->n && strcmp(ts_peek(ts), "{") != 0)
        { buf[n++] = ts->toks[ts->pos++]; if (n >= MAX_ARGS) break; }

    if (n == 0) { fprintf(stderr, "nsh: if: empty condition\n"); return NULL; }

    AstNode *then_b = parse_block(ts);
    if (!then_b) return NULL;

    AstNode *else_b = NULL;
    ts_skip_semi(ts);
    if (ts_peek(ts) && strcmp(ts_peek(ts), "else") == 0) {
        ts_next(ts);
        else_b = parse_block(ts);
    }

    AstNode *node = make_node(AST_IF);
    if (!node) { free_ast(then_b); free_ast(else_b); return NULL; }
    node->u.if_n.cond  = malloc(n * sizeof(char *));
    node->u.if_n.ncond = n;
    for (int i = 0; i < n; i++) node->u.if_n.cond[i] = strdup(buf[i]);
    node->u.if_n.then_b = then_b;
    node->u.if_n.else_b = else_b;
    return node;
}

/* ── for ─────────────────────────────────────────────────────────────────── */

static AstNode *parse_for(TS *ts)
{
    ts_next(ts);  /* consume "for" */

    const char *var = ts_next(ts);
    if (!var) { fprintf(stderr, "nsh: for: expected variable name\n"); return NULL; }
    char *var_name = strdup(var);

    /* optional "in" */
    if (ts_peek(ts) && strcmp(ts_peek(ts), "in") == 0) ts_next(ts);

    /* word list until { */
    char **words = NULL; int nwords = 0, wcap = 0;
    while (ts->pos < ts->n && strcmp(ts_peek(ts), "{") != 0) {
        if (nwords >= wcap)
            { wcap = wcap ? wcap*2 : 4; words = realloc(words, wcap * sizeof(char*)); }
        words[nwords++] = strdup(ts->toks[ts->pos++]);
    }

    AstNode *body = parse_block(ts);
    if (!body) {
        free(var_name);
        for (int i = 0; i < nwords; i++) free(words[i]);
        free(words);
        return NULL;
    }

    AstNode *node = make_node(AST_FOR);
    if (!node) { free(var_name); for(int i=0;i<nwords;i++)free(words[i]); free(words); free_ast(body); return NULL; }
    node->u.for_n.var    = var_name;
    node->u.for_n.words  = words;
    node->u.for_n.nwords = nwords;
    node->u.for_n.body   = body;
    return node;
}

/* ── while ───────────────────────────────────────────────────────────────── */

static AstNode *parse_while(TS *ts)
{
    ts_next(ts);  /* consume "while" */

    char *buf[MAX_ARGS]; int n = 0;
    while (ts->pos < ts->n && strcmp(ts_peek(ts), "{") != 0)
        { buf[n++] = ts->toks[ts->pos++]; if (n >= MAX_ARGS) break; }

    if (n == 0) { fprintf(stderr, "nsh: while: empty condition\n"); return NULL; }

    AstNode *body = parse_block(ts);
    if (!body) return NULL;

    AstNode *node = make_node(AST_WHILE);
    if (!node) { free_ast(body); return NULL; }
    node->u.while_n.cond  = malloc(n * sizeof(char *));
    node->u.while_n.ncond = n;
    for (int i = 0; i < n; i++) node->u.while_n.cond[i] = strdup(buf[i]);
    node->u.while_n.body  = body;
    return node;
}

/* ── def ─────────────────────────────────────────────────────────────────── */

static AstNode *parse_def(TS *ts)
{
    ts_next(ts);  /* consume "def" */

    const char *name = ts_next(ts);
    if (!name) { fprintf(stderr, "nsh: def: expected function name\n"); return NULL; }
    char *fname = strdup(name);

    /* parameter names until { */
    char **params = NULL; int nparams = 0, pcap = 0;
    while (ts->pos < ts->n && strcmp(ts_peek(ts), "{") != 0) {
        if (nparams >= pcap)
            { pcap = pcap ? pcap*2 : 4; params = realloc(params, pcap * sizeof(char*)); }
        params[nparams++] = strdup(ts->toks[ts->pos++]);
    }

    AstNode *body = parse_block(ts);
    if (!body) {
        free(fname);
        for (int i = 0; i < nparams; i++) free(params[i]);
        free(params);
        return NULL;
    }

    AstNode *node = make_node(AST_DEF);
    if (!node) { free(fname); for(int i=0;i<nparams;i++)free(params[i]); free(params); free_ast(body); return NULL; }
    node->u.def_n.name    = fname;
    node->u.def_n.params  = params;
    node->u.def_n.nparams = nparams;
    node->u.def_n.body    = body;
    return node;
}

/* ── statement dispatcher ────────────────────────────────────────────────── */

static AstNode *parse_stmt(TS *ts)
{
    const char *peek = ts_peek(ts);
    if (!peek) return NULL;

    if (strcmp(peek, "if")    == 0) return parse_if(ts);
    if (strcmp(peek, "for")   == 0) return parse_for(ts);
    if (strcmp(peek, "while") == 0) return parse_while(ts);
    if (strcmp(peek, "def")   == 0) return parse_def(ts);

    return parse_pipeline_ts(ts);
}

/* ── public entry point ──────────────────────────────────────────────────── */

AstNode *parse_program(char **tokens, int ntokens)
{
    TS ts = { tokens, ntokens, 0 };

    AstNode **stmts = NULL;
    int nstmts = 0, cap = 0;

    for (;;) {
        ts_skip_semi(&ts);
        if (ts.pos >= ts.n) break;

        AstNode *s = parse_stmt(&ts);
        if (!s) break;

        if (nstmts >= cap)
            { cap = cap ? cap*2 : 4; stmts = realloc(stmts, cap * sizeof(AstNode*)); }
        stmts[nstmts++] = s;
    }

    if (nstmts == 0) { free(stmts); return NULL; }

    if (nstmts == 1) {
        AstNode *single = stmts[0];
        free(stmts);
        return single;
    }

    AstNode *node = make_node(AST_BLOCK);
    if (!node) { for(int i=0;i<nstmts;i++) free_ast(stmts[i]); free(stmts); return NULL; }
    node->u.block.stmts  = stmts;
    node->u.block.nstmts = nstmts;
    return node;
}

/* ── AST memory ──────────────────────────────────────────────────────────── */

void free_ast(AstNode *node)
{
    if (!node) return;
    switch (node->kind) {
    case AST_PIPELINE:
        for (int i = 0; i < node->u.pipeline.n; i++) free(node->u.pipeline.raw[i]);
        free(node->u.pipeline.raw);
        break;
    case AST_BLOCK:
        for (int i = 0; i < node->u.block.nstmts; i++)
            free_ast(node->u.block.stmts[i]);
        free(node->u.block.stmts);
        break;
    case AST_IF:
        for (int i = 0; i < node->u.if_n.ncond; i++) free(node->u.if_n.cond[i]);
        free(node->u.if_n.cond);
        free_ast(node->u.if_n.then_b);
        free_ast(node->u.if_n.else_b);
        break;
    case AST_FOR:
        free(node->u.for_n.var);
        for (int i = 0; i < node->u.for_n.nwords; i++) free(node->u.for_n.words[i]);
        free(node->u.for_n.words);
        free_ast(node->u.for_n.body);
        break;
    case AST_WHILE:
        for (int i = 0; i < node->u.while_n.ncond; i++) free(node->u.while_n.cond[i]);
        free(node->u.while_n.cond);
        free_ast(node->u.while_n.body);
        break;
    case AST_DEF:
        free(node->u.def_n.name);
        for (int i = 0; i < node->u.def_n.nparams; i++) free(node->u.def_n.params[i]);
        free(node->u.def_n.params);
        /* body ownership transferred to ExecCtx — freed by execctx_free() */
        break;
    }
    free(node);
}
