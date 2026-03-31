#include "shell.h"

/*
 * Inline command prediction (fish-shell style).
 *
 * As the user types, the most recent history entry that starts with the
 * current buffer is shown as dim/grey ghost text to the right of the cursor.
 *
 *   → or Ctrl+F  — accept the full suggestion
 *   Alt+→ / Alt+F — accept one word of the suggestion
 *   Any other key — ghost disappears, key behaves normally
 *
 * Implementation:
 *   We hook rl_redisplay_function.  After the real rl_redisplay() draws the
 *   prompt + buffer (cursor now at rl_point), we use DEC save/restore cursor
 *   (\0337 / \0338) to:
 *     1. Save the cursor position readline left it at.
 *     2. Erase everything to the right (\033[J) — removes the previous ghost.
 *     3. Print the new ghost in dim colour.
 *     4. Restore the cursor to the saved position.
 *
 *   Because we save/restore the cursor, readline's internal state never
 *   disagrees with the real terminal cursor position.  No column arithmetic
 *   is needed.  We still truncate to one terminal line to prevent the ghost
 *   from scrolling the screen.
 */

static char ghost_suffix[NSH_MAX_INPUT] = "";
static char last_buffer[NSH_MAX_INPUT]  = "";

/* ── find the best match in readline's in-memory history ring ────────────── */

static void update_ghost(void)
{
    if (!rl_line_buffer) { ghost_suffix[0] = '\0'; return; }

    /* only predict when cursor is at end of line */
    if (rl_point != rl_end) { ghost_suffix[0] = '\0'; return; }

    /* skip work if buffer hasn't changed */
    if (strcmp(last_buffer, rl_line_buffer) == 0) return;

    strncpy(last_buffer, rl_line_buffer, sizeof(last_buffer) - 1);
    last_buffer[sizeof(last_buffer) - 1] = '\0';

    ghost_suffix[0] = '\0';
    if (rl_line_buffer[0] == '\0') return;

    int len = (int)strlen(rl_line_buffer);

    for (int i = history_length; i >= 1; i--) {
        HIST_ENTRY *e = history_get(i);
        if (!e || !e->line) continue;
        if (strncmp(e->line, rl_line_buffer, len) == 0 &&
            (int)strlen(e->line) > len) {
            strncpy(ghost_suffix, e->line + len, sizeof(ghost_suffix) - 1);
            ghost_suffix[sizeof(ghost_suffix) - 1] = '\0';
            return;
        }
    }
}

/* ── custom redisplay ────────────────────────────────────────────────────── */

static void predict_redisplay(void)
{
    update_ghost();

    /* let readline draw the prompt + buffer; cursor lands at rl_point */
    rl_redisplay_function = NULL;
    rl_redisplay();
    rl_redisplay_function = predict_redisplay;

    /*
     * \0337  — DEC save cursor (supported by xterm, iTerm2, Terminal.app,
     *           tmux, and every other modern terminal emulator)
     * \033[J — erase from cursor to end of screen (clears old ghost,
     *           even if it had wrapped onto the line below)
     */
    fprintf(rl_outstream, "\0337\033[J");

    if (!ghost_suffix[0]) {
        fflush(rl_outstream);
        return;
    }

    /*
     * Truncate the ghost to at most (terminal_width - 1) characters so it
     * never causes the terminal to scroll, which would shift all line
     * positions and corrupt readline's layout.
     */
    int rows, cols;
    rl_get_screen_size(&rows, &cols);
    if (cols <= 1) cols = 80;

    int glen = (int)strlen(ghost_suffix);
    int show = glen < (cols - 1) ? glen : (cols - 1);

    /* dim grey ghost, then restore cursor to where readline left it */
    fprintf(rl_outstream, "\033[2m%.*s\033[0m\0338", show, ghost_suffix);
    fflush(rl_outstream);
}

/* ── key handlers ────────────────────────────────────────────────────────── */

static int accept_or_forward(int count, int key)
{
    if (ghost_suffix[0] && rl_point == rl_end) {
        rl_insert_text(ghost_suffix);
        ghost_suffix[0] = '\0';
        last_buffer[0]  = '\0';
        return 0;
    }
    return rl_forward_char(count, key);
}

static int accept_word(int count, int key)
{
    (void)count; (void)key;
    if (!ghost_suffix[0] || rl_point != rl_end)
        return rl_forward_word(count, key);

    const char *p = ghost_suffix;
    while (*p && !isalnum((unsigned char)*p) && *p != '_') p++;
    while (*p &&  (isalnum((unsigned char)*p) || *p == '_')) p++;

    int n = (int)(p - ghost_suffix);
    if (n == 0) n = 1;

    char tmp[NSH_MAX_INPUT];
    strncpy(tmp, ghost_suffix, n);
    tmp[n] = '\0';

    rl_insert_text(tmp);
    memmove(ghost_suffix, ghost_suffix + n, strlen(ghost_suffix) - n + 1);
    last_buffer[0] = '\0';
    return 0;
}

static int dismiss_ghost(int count, int key)
{
    (void)count; (void)key;
    ghost_suffix[0] = '\0';
    last_buffer[0]  = '\0';
    rl_forced_update_display();
    return 0;
}

/*
 * ctrl_c_handler — Ctrl+C clears the current input line and prints "^C".
 * The shell ignores SIGINT (so Ctrl+C won't kill it), which also means
 * readline's built-in Ctrl+C behaviour is disabled.  We restore a useful
 * action: abandon the current line and give a fresh prompt.
 */
static int ctrl_c_handler(int count, int key)
{
    (void)count; (void)key;
    ghost_suffix[0] = '\0';
    last_buffer[0]  = '\0';
    rl_replace_line("", 0);         /* clear the buffer cleanly */
    rl_point = 0;
    fprintf(rl_outstream, "^C\n");
    fflush(rl_outstream);
    rl_on_new_line();
    rl_forced_update_display();
    return 0;
}

/* ── init ────────────────────────────────────────────────────────────────── */

void predict_init(void)
{
    rl_redisplay_function = predict_redisplay;

    rl_bind_keyseq("\033[C",   accept_or_forward);  /* right arrow (VT)  */
    rl_bind_keyseq("\033OC",   accept_or_forward);  /* right arrow (app) */
    rl_bind_key(6,             accept_or_forward);  /* Ctrl+F            */

    rl_bind_keyseq("\033[1;3C", accept_word);       /* Alt+right (xterm) */
    rl_bind_keyseq("\033[3C",   accept_word);       /* Alt+right (other) */
    rl_bind_keyseq("\033f",     accept_word);       /* Alt+F             */

    /* explicitly restore left arrow so our right-arrow bindings don't
     * confuse readline's sequence parser and cause \033[D to misfire */
    rl_bind_keyseq("\033[D",   rl_named_function("backward-char"));
    rl_bind_keyseq("\033OD",   rl_named_function("backward-char"));

    rl_bind_key(3,             ctrl_c_handler);     /* Ctrl+C           */
    rl_bind_keyseq("\033[27~", dismiss_ghost);       /* Escape           */
}
