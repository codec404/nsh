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
