#include "shell.h"

void line_editor_init(void) {
    complete_init();
}

void line_editor_shutdown(void) {}

char *line_editor_read(const char *prompt) {
    return readline(prompt ? prompt : "");
}

void line_editor_add_history(const char *line) {
    if (!line || !*line) return;
    HIST_ENTRY *prev = history_get(history_length);
    if (!prev || strcmp(prev->line, line) != 0) add_history(line);
}

int line_editor_was_interrupted(void) {
    return 0;
}

void line_editor_clear_interrupt(void) {}
