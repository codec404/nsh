#include "shell.h"
#include <stdint.h>

/* ── globals ─────────────────────────────────────────────────────────────── */

ExecCtx    g_ctx;
UnwindKind nsh_unwind = UNWIND_NONE;
int        nsh_retval  = 0;

/* ── context lifecycle ───────────────────────────────────────────────────── */

void execctx_init(ExecCtx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

static void scope_free_one(Scope *s)
{
    for (int i = 0; i < s->n; i++) { free(s->names[i]); free(s->vals[i]); }
    free(s->names); free(s->vals);
    memset(s, 0, sizeof(*s));
}

void execctx_free(ExecCtx *ctx)
{
    for (int i = 0; i < ctx->nscopes; i++) scope_free_one(&ctx->scopes[i]);
    free(ctx->scopes);

    for (int i = 0; i < ctx->nfuncs; i++) {
        free(ctx->funcs[i].name);
        for (int j = 0; j < ctx->funcs[i].nparams; j++) free(ctx->funcs[i].params[j]);
        free(ctx->funcs[i].params);
        free_ast(ctx->funcs[i].body);
    }
    free(ctx->funcs);
    memset(ctx, 0, sizeof(*ctx));
}

/* ── scope management ────────────────────────────────────────────────────── */

static void push_scope(ExecCtx *ctx)
{
    if (ctx->nscopes >= ctx->scope_cap) {
        ctx->scope_cap = ctx->scope_cap ? ctx->scope_cap * 2 : 4;
        ctx->scopes = realloc(ctx->scopes, ctx->scope_cap * sizeof(Scope));
    }
    memset(&ctx->scopes[ctx->nscopes++], 0, sizeof(Scope));
}

static void pop_scope(ExecCtx *ctx)
{
    if (ctx->nscopes <= 0) return;
    Scope *s = &ctx->scopes[--ctx->nscopes];
    for (int i = 0; i < s->n; i++) unsetenv(s->names[i]);
    scope_free_one(s);
}

static void scope_set(ExecCtx *ctx, const char *name, const char *val)
{
    if (!name) return;
    if (!val) val = "";

    if (ctx->nscopes > 0) {
        Scope *s = &ctx->scopes[ctx->nscopes - 1];
        for (int i = 0; i < s->n; i++) {
            if (strcmp(s->names[i], name) == 0) {
                free(s->vals[i]);
                s->vals[i] = strdup(val);
                setenv(name, val, 1);
                return;
            }
        }
        if (s->n >= s->cap) {
            s->cap = s->cap ? s->cap * 2 : 4;
            s->names = realloc(s->names, s->cap * sizeof(char *));
            s->vals  = realloc(s->vals,  s->cap * sizeof(char *));
        }
        s->names[s->n] = strdup(name);
        s->vals[s->n]  = strdup(val);
        s->n++;
    }
    setenv(name, val, 1);
}

/* ── function lookup / call ──────────────────────────────────────────────── */

static FuncDef *find_func(ExecCtx *ctx, const char *name)
{
    for (int i = ctx->nfuncs - 1; i >= 0; i--)
        if (strcmp(ctx->funcs[i].name, name) == 0)
            return &ctx->funcs[i];
    return NULL;
}

static int call_function(FuncDef *fd, char **args, int nargs, ExecCtx *ctx)
{
    push_scope(ctx);

    /* named params */
    for (int i = 0; i < fd->nparams; i++)
        scope_set(ctx, fd->params[i], i < nargs ? args[i] : "");

    /* positional $1 $2 ... and $# */
    for (int i = 0; i < nargs; i++) {
        char pos[16]; snprintf(pos, sizeof(pos), "%d", i + 1);
        scope_set(ctx, pos, args[i]);
    }
    char nbuf[16]; snprintf(nbuf, sizeof(nbuf), "%d", nargs);
    scope_set(ctx, "#", nbuf);

    int status = execute_ast(fd->body, ctx);

    if (nsh_unwind == UNWIND_RETURN) { status = nsh_retval; nsh_unwind = UNWIND_NONE; }

    pop_scope(ctx);
    return status;
}

/* ── helper: expand raw tokens → build → execute pipeline ───────────────── */

static int run_raw_pipeline(char **raw, int n, ExecCtx *ctx)
{
    char **exp = expand_argv(raw, n);
    if (!exp) return 1;

    int nexp = 0; while (exp[nexp]) nexp++;
    if (nexp == 0) { free(exp); return 0; }

    /* user-defined function? */
    if (ctx) {
        FuncDef *fd = find_func(ctx, exp[0]);
        if (fd) {
            int st = call_function(fd, exp + 1, nexp - 1, ctx);
            free_tokens(exp);
            return st;
        }
    }

    Pipeline *p = parse_pipeline(exp, nexp, NULL);
    free_tokens(exp);
    if (!p) return 1;
    int st = execute_pipeline(p);
    free_pipeline(p);
    last_status = st;
    return st;
}

/* ── main AST executor ───────────────────────────────────────────────────── */

int execute_ast(AstNode *node, ExecCtx *ctx)
{
    if (!node) return 0;

    switch (node->kind) {

    /* ── pipeline: expand raw tokens at call time ── */
    case AST_PIPELINE:
        return run_raw_pipeline(node->u.pipeline.raw, node->u.pipeline.n, ctx);

    /* ── block ── */
    case AST_BLOCK: {
        int status = 0;
        for (int i = 0; i < node->u.block.nstmts; i++) {
            status = execute_ast(node->u.block.stmts[i], ctx);
            last_status = status;
            if (nsh_unwind != UNWIND_NONE) break;
        }
        return status;
    }

    /* ── if: expand condition at call time ── */
    case AST_IF: {
        char **exp = expand_argv(node->u.if_n.cond, node->u.if_n.ncond);
        int n = 0; while (exp[n]) n++;
        Pipeline *cp = n > 0 ? parse_pipeline(exp, n, NULL) : NULL;
        free_tokens(exp);
        int cond = cp ? execute_pipeline(cp) : 1;
        if (cp) free_pipeline(cp);
        last_status = cond;
        if (cond == 0)              return execute_ast(node->u.if_n.then_b, ctx);
        if (node->u.if_n.else_b)   return execute_ast(node->u.if_n.else_b, ctx);
        return 0;
    }

    /* ── for: expand each word at iteration time ── */
    case AST_FOR: {
        int status = 0;
        push_scope(ctx);
        for (int i = 0; i < node->u.for_n.nwords; i++) {
            char *w = expand_word(node->u.for_n.words[i]);
            scope_set(ctx, node->u.for_n.var, w);
            free(w);
            status = execute_ast(node->u.for_n.body, ctx);
            last_status = status;
            if (nsh_unwind == UNWIND_BREAK)    { nsh_unwind = UNWIND_NONE; break; }
            if (nsh_unwind == UNWIND_CONTINUE) { nsh_unwind = UNWIND_NONE; continue; }
            if (nsh_unwind == UNWIND_RETURN)   break;
        }
        pop_scope(ctx);
        return status;
    }

    /* ── while: expand condition each iteration ── */
    case AST_WHILE: {
        int status = 0;
        push_scope(ctx);
        for (;;) {
            char **exp = expand_argv(node->u.while_n.cond, node->u.while_n.ncond);
            int n = 0; while (exp[n]) n++;
            Pipeline *cp = n > 0 ? parse_pipeline(exp, n, NULL) : NULL;
            free_tokens(exp);
            int cond = cp ? execute_pipeline(cp) : 1;
            if (cp) free_pipeline(cp);
            if (cond != 0) break;
            status = execute_ast(node->u.while_n.body, ctx);
            last_status = status;
            if (nsh_unwind == UNWIND_BREAK)    { nsh_unwind = UNWIND_NONE; break; }
            if (nsh_unwind == UNWIND_CONTINUE) { nsh_unwind = UNWIND_NONE; continue; }
            if (nsh_unwind == UNWIND_RETURN)   break;
        }
        pop_scope(ctx);
        return status;
    }

    /* ── def: register function ── */
    case AST_DEF: {
        if (ctx->nfuncs >= ctx->func_cap) {
            ctx->func_cap = ctx->func_cap ? ctx->func_cap * 2 : 4;
            ctx->funcs = realloc(ctx->funcs, ctx->func_cap * sizeof(FuncDef));
        }
        /* replace existing definition with same name */
        for (int i = 0; i < ctx->nfuncs; i++) {
            if (strcmp(ctx->funcs[i].name, node->u.def_n.name) == 0) {
                free(ctx->funcs[i].name);
                for (int j = 0; j < ctx->funcs[i].nparams; j++) free(ctx->funcs[i].params[j]);
                free(ctx->funcs[i].params);
                free_ast(ctx->funcs[i].body);
                ctx->funcs[i] = ctx->funcs[--ctx->nfuncs];
                break;
            }
        }
        FuncDef *fd = &ctx->funcs[ctx->nfuncs++];
        fd->name    = strdup(node->u.def_n.name);
        fd->nparams = node->u.def_n.nparams;
        fd->params  = NULL;
        if (fd->nparams > 0) {
            fd->params = malloc(fd->nparams * sizeof(char *));
            for (int i = 0; i < fd->nparams; i++)
                fd->params[i] = strdup(node->u.def_n.params[i]);
        }
        fd->body = node->u.def_n.body;   /* ownership transferred */
        return 0;
    }

    } /* switch */
    return 0;
}

/* ── script file execution ───────────────────────────────────────────────── */

int execute_script_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "nsh: %s: %s\n", path, strerror(errno)); return 1; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)size + 2);
    if (!buf) { fclose(f); return 1; }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got]     = '\n';
    buf[got + 1] = '\0';

    int ntokens = 0;
    char **tokens = tokenize_raw(buf, &ntokens);
    free(buf);

    if (!tokens || ntokens == 0) { free_tokens(tokens); return 0; }

    AstNode *ast = parse_program(tokens, ntokens);
    free_tokens(tokens);
    if (!ast) return 1;

    int status = execute_ast(ast, &g_ctx);
    if (nsh_unwind == UNWIND_RETURN) { status = nsh_retval; nsh_unwind = UNWIND_NONE; }

    free_ast(ast);
    return status;
}
