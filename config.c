#include "shell.h"

NshConfig g_config = {
    .prompt_format  = "%u:%w%g%j%e%s ",
    .prompt_show_ms = 500,
    .history_size   = 1000,
};

static char *cfg_trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\n' || end[-1] == '\r'))
        *--end = '\0';
    return s;
}

static void apply_kv(const char *section, const char *key, const char *val)
{
    char full[128];
    if (section[0])
        snprintf(full, sizeof(full), "%s.%s", section, key);
    else
        snprintf(full, sizeof(full), "%s", key);

    if (strcmp(full, "prompt.format") == 0)
        snprintf(g_config.prompt_format, sizeof(g_config.prompt_format), "%s", val);
    else if (strcmp(full, "prompt.show_duration_ms") == 0)
        g_config.prompt_show_ms = atoi(val);
    else if (strcmp(full, "history.size") == 0)
        g_config.history_size = atoi(val);
}

/*
 * config_load — reads ~/.config/nsh/config.toml if it exists.
 *
 * Supported format:
 *
 *   [prompt]
 *   format           = "%u:%w%g%j%e%s "
 *   show_duration_ms = 500          # show if command took >= N ms; -1 = never
 *
 *   [history]
 *   size = 1000
 *
 * Lines starting with # are comments.  Values may be quoted with "".
 * Inline # comments are stripped from unquoted values.
 */
void config_load(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/nsh/config.toml", home);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    char section[64] = "";

    while (fgets(line, sizeof(line), f)) {
        char *s = cfg_trim(line);
        if (*s == '#' || *s == '\0') continue;

        /* section header */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) { *end = '\0'; snprintf(section, sizeof(section), "%s", s + 1); }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = cfg_trim(s);
        char *val = cfg_trim(eq + 1);

        /* strip quotes */
        size_t vlen = strlen(val);
        int quoted = (vlen >= 2 && val[0] == '"' && val[vlen-1] == '"');
        if (quoted) { val[vlen-1] = '\0'; val++; }

        /* strip inline comment for unquoted values */
        if (!quoted) {
            char *cmt = strchr(val, '#');
            if (cmt) {
                *cmt = '\0';
                char *v = val + strlen(val);
                while (v > val && (v[-1] == ' ' || v[-1] == '\t')) *--v = '\0';
            }
        }

        apply_kv(section, key, val);
    }

    fclose(f);
}
