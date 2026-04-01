#ifndef SHELL_H
#define SHELL_H

#include "value.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <limits.h>
#include <termios.h>
#include <setjmp.h>

#define MAX_ARGS      128
#define NSH_MAX_INPUT 4096
#define MAX_JOBS       64

/* ── data structures ────────────────────────────────────────────────────── */

typedef struct {
    char **argv;
    int    argc;
    char  *redir_in;
    char  *redir_out;
    int    append_out;
} Cmd;

typedef struct {
    Cmd  *cmds;
    int   ncmds;
    int   background;   /* 1 if trailing & */
    char *cmdline;      /* original text, for job display */
} Pipeline;

/* ── AST for scripting ──────────────────────────────────────────────────── */

typedef enum {
    AST_PIPELINE,   /* a plain pipeline */
    AST_BLOCK,      /* { stmt; stmt; ... } */
    AST_IF,         /* if pipeline { block } [else { block }] */
    AST_FOR,        /* for var in words { block } */
    AST_WHILE,      /* while pipeline { block } */
    AST_DEF,        /* def name [params] { block } */
} AstKind;

typedef struct AstNode AstNode;

/*
 * All pipeline/condition slots store RAW (unexpanded) token arrays.
 * Expansion ($VAR, ~, $()) happens at execution time in execute_ast()
 * via expand_argv() / expand_word(), so function bodies see the correct
 * values of their parameters.
 */
struct AstNode {
    AstKind kind;
    union {
        struct { char **raw; int n; }                                           pipeline;
        struct { AstNode **stmts; int nstmts; }                                block;
        struct { char **cond; int ncond; AstNode *then_b; AstNode *else_b; }  if_n;
        struct { char *var; char **words; int nwords; AstNode *body; }         for_n;
        struct { char **cond; int ncond; AstNode *body; }                      while_n;
        struct { char *name; char **params; int nparams; AstNode *body; }      def_n;
    } u;
};

/* ── execution context (scripting) ─────────────────────────────────────── */

typedef struct {
    char **names;
    char **vals;
    int    n, cap;
} Scope;

typedef struct {
    char    *name;
    char   **params;
    int      nparams;
    AstNode *body;   /* owned by this FuncDef */
} FuncDef;

typedef struct {
    Scope   *scopes;    int nscopes,  scope_cap;
    FuncDef *funcs;     int nfuncs,   func_cap;
} ExecCtx;

/* unwind signals for break / continue / return */
typedef enum { UNWIND_NONE=0, UNWIND_BREAK, UNWIND_CONTINUE, UNWIND_RETURN } UnwindKind;
extern UnwindKind nsh_unwind;
extern int        nsh_retval;

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } JobState;

typedef struct {
    int       num;          /* job number shown to user, 1-based */
    pid_t     pgid;         /* process group id of the pipeline */
    pid_t    *pids;         /* individual pids in the pipeline */
    int       npids;
    int       ndone;        /* how many pids have exited/stopped */
    JobState  state;
    char     *cmdline;
    int       notified;     /* have we printed completion to user? */
} Job;

/* ── lexer.c ────────────────────────────────────────────────────────────── */
char     **tokenize(char *line, int *ntokens);
char     **tokenize_raw(char *line, int *ntokens);  /* no $-expansion */
char      *expand_word(const char *raw);            /* expand one raw token */
char     **expand_argv(char **raw, int n);          /* expand array of raw tokens */
void       free_tokens(char **tokens);

/* ── parser.c ───────────────────────────────────────────────────────────── */
Pipeline  *parse_pipeline(char **tokens, int ntokens, const char *cmdline);
void       free_pipeline(Pipeline *p);
AstNode   *parse_program(char **tokens, int ntokens);
void       free_ast(AstNode *node);

/* ── script.c ───────────────────────────────────────────────────────────── */
void execctx_init(ExecCtx *ctx);
void execctx_free(ExecCtx *ctx);
int  execute_ast(AstNode *node, ExecCtx *ctx);
int  execute_script_file(const char *path);
extern ExecCtx g_ctx;

/* ── jobs.c ─────────────────────────────────────────────────────────────── */
void  jobs_init(void);
Job  *jobs_add(pid_t pgid, pid_t *pids, int npids, const char *cmdline);
void  jobs_set_state(Job *j, JobState state);
Job  *jobs_find_num(int num);
Job  *jobs_find_pgid(pid_t pgid);
Job  *jobs_last(void);          /* most recently added/stopped, for fg/bg */
void  jobs_print_all(void);
void  reap_jobs(void);          /* call before each prompt to reap background jobs */
int   jobs_count(void);         /* current number of tracked jobs */

/* ── executor.c ─────────────────────────────────────────────────────────── */
int   execute_pipeline(Pipeline *p);
int   wait_foreground(Job *j);  /* used by fg builtin */

/* ── history.c ──────────────────────────────────────────────────────────── */
#include <stdint.h>
void      hist_open(void);
void      hist_close(void);
void      hist_add(const char *cmdline, int exit_code, int64_t duration_ms);
void      hist_seed_interactive(void);
NshTable *hist_query(char **argv, int argc);
NshTable *hist_sessions(void);
char    **hist_session_cmds(int64_t session_id, int *out_n);

/* ── env_config.c ───────────────────────────────────────────────────────── */
void shellenv_init(void);
void shellenv_shutdown(void);
void shellenv_cd_hook(const char *new_cwd);
void shellenv_check_reload(void);
void shellenv_diff(const char *target_dir);   /* NULL = show active only */
void shellenv_show(void);
int  shellenv_link(const char *target);       /* NULL = create template */
int  shellenv_depth(void);                    /* current env stack depth */

/* ── builtins.c ─────────────────────────────────────────────────────────── */
int   is_builtin(const char *cmd);
int   run_builtin(char **argv, int argc);

/* ── config.c ───────────────────────────────────────────────────────────── */
typedef struct {
    char prompt_format[256]; /* format tokens: %u %w %g %j %e %s %% */
    int  prompt_show_ms;     /* print duration if >= N ms; -1 = never */
    int  history_size;       /* readline history ring size */
} NshConfig;

extern NshConfig g_config;
void config_load(void);

/* ── prompt.c ───────────────────────────────────────────────────────────── */
char *make_prompt(void);

/* ── complete.c ─────────────────────────────────────────────────────────── */
void complete_init(void);

/* ── line_editor.c ──────────────────────────────────────────────────────── */
void  line_editor_init(void);
void  line_editor_shutdown(void);
char *line_editor_read(const char *prompt);   /* malloc'd string, NULL on EOF */
void  line_editor_add_history(const char *line);
int   line_editor_was_interrupted(void);       /* Ctrl+C during edit */
void  line_editor_clear_interrupt(void);

/* ── error.c ────────────────────────────────────────────────────────────── */
void suggest_command_not_found(const char *cmd);
void suggest_on_error(const char *file, int err);

/* ── globals ────────────────────────────────────────────────────────────── */
extern int                    last_status;
extern pid_t                  shell_pgid;
extern pid_t                  last_bg_pgid;   /* for $! expansion */
extern volatile sig_atomic_t  got_sigchld;

#endif
