#include "shell.h"
#include <sqlite3.h>
#include <time.h>

/* ── schema ─────────────────────────────────────────────────────────────── */

/*
 * Table: history
 *   id          INTEGER PRIMARY KEY AUTOINCREMENT
 *   session_id  INTEGER  — shell PID; groups one interactive session
 *   cmdline     TEXT
 *   exit_code   INTEGER
 *   duration_ms INTEGER  — wall-clock ms measured around execute_pipeline()
 *   cwd         TEXT
 *   ts          INTEGER  — Unix timestamp (seconds)
 */
#define HIST_DB_FILE "/.nsh_history.db"
#define HIST_MAX_READLINE 1000   /* entries loaded into readline ring buffer */

static sqlite3 *g_db      = NULL;
static int64_t  g_session = 0;   /* = getpid() of this shell */

/* ── open / init ────────────────────────────────────────────────────────── */

void hist_open(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", home, HIST_DB_FILE);

    if (sqlite3_open(path, &g_db) != SQLITE_OK) {
        fprintf(stderr, "nsh: history db: %s\n", sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return;
    }

    /* WAL mode: better concurrency, faster writes */
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS history ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id  INTEGER  NOT NULL,"
        "  cmdline     TEXT     NOT NULL,"
        "  exit_code   INTEGER  NOT NULL DEFAULT 0,"
        "  duration_ms INTEGER  NOT NULL DEFAULT 0,"
        "  cwd         TEXT     NOT NULL DEFAULT '',"
        "  ts          INTEGER  NOT NULL DEFAULT 0"
        ");";

    char *err = NULL;
    if (sqlite3_exec(g_db, create_sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "nsh: history db init: %s\n", err);
        sqlite3_free(err);
    }

    /* index for fast --failed / --since queries */
    sqlite3_exec(g_db,
        "CREATE INDEX IF NOT EXISTS idx_hist_exit ON history(exit_code);",
        NULL, NULL, NULL);
    sqlite3_exec(g_db,
        "CREATE INDEX IF NOT EXISTS idx_hist_ts ON history(ts);",
        NULL, NULL, NULL);
    sqlite3_exec(g_db,
        "CREATE INDEX IF NOT EXISTS idx_hist_session ON history(session_id);",
        NULL, NULL, NULL);

    g_session = (int64_t)getpid();

    /*
     * Seed readline's in-memory ring with the most recent HIST_MAX_READLINE
     * commands.  Arrow-up will now recall them from SQLite, not just the
     * flat ~/.nsh_history file (which we no longer write).
     */
    const char *seed_sql =
        "SELECT cmdline FROM history"
        "  ORDER BY id DESC LIMIT ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, seed_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, HIST_MAX_READLINE);

        /* collect in reverse so oldest is add_history()'d first */
        char *buf[HIST_MAX_READLINE];
        int   nbuf = 0;

        while (sqlite3_step(stmt) == SQLITE_ROW && nbuf < HIST_MAX_READLINE) {
            const char *s = (const char *)sqlite3_column_text(stmt, 0);
            buf[nbuf++]   = s ? strdup(s) : NULL;
        }
        sqlite3_finalize(stmt);

        /* add oldest → newest so arrow-up gives newest first */
        for (int i = nbuf - 1; i >= 0; i--) {
            if (buf[i]) { add_history(buf[i]); free(buf[i]); }
        }
    }
}

void hist_close(void)
{
    if (g_db) { sqlite3_close(g_db); g_db = NULL; }
}

/* ── record a command ───────────────────────────────────────────────────── */

void hist_add(const char *cmdline, int exit_code, int64_t duration_ms)
{
    if (!g_db || !cmdline) return;

    char cwd[PATH_MAX] = "";
    getcwd(cwd, sizeof(cwd));

    const char *sql =
        "INSERT INTO history(session_id,cmdline,exit_code,duration_ms,cwd,ts)"
        " VALUES(?,?,?,?,?,?);";
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;

    sqlite3_bind_int64(stmt, 1, g_session);
    sqlite3_bind_text (stmt, 2, cmdline,     -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 3, exit_code);
    sqlite3_bind_int64(stmt, 4, duration_ms);
    sqlite3_bind_text (stmt, 5, cwd,         -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, (int64_t)time(NULL));

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/* ── query → NshTable ───────────────────────────────────────────────────── */

/*
 * hist_query — run a SELECT against the history table and return the results
 * as a typed NshTable.
 *
 * Flags:
 *   --failed          WHERE exit_code != 0
 *   --session         WHERE session_id = current session
 *   --since <date>    WHERE ts >= strptime(date) — accepts YYYY-MM-DD or
 *                     relative offsets like "1h", "30m", "2d"
 *   --touched <path>  WHERE cwd = dirname(realpath(path))
 *                     (best-effort: matches commands run in same directory)
 *   N (bare integer)  LIMIT N  (default 100)
 *
 * Columns: id, session_id, cmdline, exit_code, duration_ms, cwd, ts
 */

static int64_t parse_since(const char *s)
{
    time_t now = time(NULL);
    /* relative: "1h", "30m", "2d", "1w" */
    char *end;
    double val = strtod(s, &end);
    if (end != s && *end) {
        switch (*end) {
        case 'm': case 'M': return (int64_t)(now - (time_t)(val * 60));
        case 'h': case 'H': return (int64_t)(now - (time_t)(val * 3600));
        case 'd': case 'D': return (int64_t)(now - (time_t)(val * 86400));
        case 'w': case 'W': return (int64_t)(now - (time_t)(val * 604800));
        }
    }
    /* absolute: YYYY-MM-DD */
    struct tm tm_s;
    memset(&tm_s, 0, sizeof(tm_s));
    if (strptime(s, "%Y-%m-%d", &tm_s))
        return (int64_t)mktime(&tm_s);
    return 0;
}

NshTable *hist_query(char **argv, int argc)
{
    if (!g_db) {
        fprintf(stderr, "history: database not open\n");
        return NULL;
    }

    int   failed_only  = 0;
    int   this_session = 0;
    int64_t since_ts   = 0;
    char   *touched    = NULL;
    int     limit      = 100;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--failed") == 0) {
            failed_only = 1;
        } else if (strcmp(argv[i], "--session") == 0) {
            this_session = 1;
        } else if (strcmp(argv[i], "--since") == 0 && i + 1 < argc) {
            since_ts = parse_since(argv[++i]);
        } else if (strcmp(argv[i], "--touched") == 0 && i + 1 < argc) {
            touched = argv[++i];
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            limit = atoi(argv[i]);
        }
    }

    /* build WHERE clause */
    char where[512] = "WHERE 1=1";
    if (failed_only)  strcat(where, " AND exit_code != 0");
    if (this_session) {
        char buf[64];
        snprintf(buf, sizeof(buf), " AND session_id = %lld", (long long)g_session);
        strcat(where, buf);
    }
    if (since_ts) {
        char buf[64];
        snprintf(buf, sizeof(buf), " AND ts >= %lld", (long long)since_ts);
        strcat(where, buf);
    }
    if (touched) {
        char real[PATH_MAX];
        char *dir = realpath(touched, real) ? strdup(real) : strdup(touched);
        /* strip filename if it's a file, keep directory */
        struct stat st;
        if (stat(dir, &st) == 0 && !S_ISDIR(st.st_mode)) {
            char *slash = strrchr(dir, '/');
            if (slash) *slash = '\0';
        }
        char buf[PATH_MAX + 32];
        snprintf(buf, sizeof(buf), " AND cwd = '%s'", dir);
        strcat(where, buf);
        free(dir);
    }

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT id, session_id, cmdline, exit_code, duration_ms, cwd, ts"
        " FROM history %s ORDER BY id DESC LIMIT %d;",
        where, limit);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "history: %s\n", sqlite3_errmsg(g_db));
        return NULL;
    }

    char *cols[] = { "id", "session", "cmdline", "exit_code", "duration_ms", "cwd", "ts" };
    NshTable *t = table_new(cols, 7);
    if (!t) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        /* ts → human readable */
        int64_t ts = sqlite3_column_int64(stmt, 6);
        struct tm *tm_info = localtime((time_t *)&ts);
        char ts_buf[32];
        strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M", tm_info);

        NshVal vals[7] = {
            val_int  ((int64_t)sqlite3_column_int64(stmt, 0)),
            val_int  ((int64_t)sqlite3_column_int64(stmt, 1)),
            val_str  ((const char *)sqlite3_column_text(stmt, 2)),
            val_int  ((int64_t)sqlite3_column_int(stmt,  3)),
            val_int  ((int64_t)sqlite3_column_int64(stmt, 4)),
            val_str  ((const char *)sqlite3_column_text(stmt, 5)),
            val_str  (ts_buf),
        };
        table_append(t, vals);
        for (int i = 0; i < 7; i++) val_free(&vals[i]);
    }

    sqlite3_finalize(stmt);
    return t;
}

/* ── replay: return commands for a session as a string array ────────────── */

/*
 * hist_session_cmds — return all cmdlines for a given session_id,
 * oldest first.  Caller must free() each string and the array.
 */
char **hist_session_cmds(int64_t session_id, int *out_n)
{
    if (!g_db) { *out_n = 0; return NULL; }

    const char *sql =
        "SELECT cmdline FROM history"
        " WHERE session_id = ?"
        " ORDER BY id ASC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        *out_n = 0; return NULL;
    }
    sqlite3_bind_int64(stmt, 1, session_id);

    int cap = 64, n = 0;
    char **cmds = malloc(cap * sizeof(char *));
    if (!cmds) { sqlite3_finalize(stmt); *out_n = 0; return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            char **tmp = realloc(cmds, cap * sizeof(char *));
            if (!tmp) break;
            cmds = tmp;
        }
        const char *s = (const char *)sqlite3_column_text(stmt, 0);
        cmds[n++] = s ? strdup(s) : strdup("");
    }
    sqlite3_finalize(stmt);
    *out_n = n;
    return cmds;
}

/* ── list distinct session IDs ──────────────────────────────────────────── */

NshTable *hist_sessions(void)
{
    if (!g_db) return NULL;

    const char *sql =
        "SELECT session_id,"
        "       COUNT(*) as cmds,"
        "       MIN(ts) as started,"
        "       MAX(ts) as ended"
        " FROM history"
        " GROUP BY session_id"
        " ORDER BY started DESC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;

    char *cols[] = { "session_id", "cmds", "started", "ended" };
    NshTable *t = table_new(cols, 4);
    if (!t) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t ts_s = sqlite3_column_int64(stmt, 2);
        int64_t ts_e = sqlite3_column_int64(stmt, 3);
        char s_buf[32], e_buf[32];
        struct tm *tms = localtime((time_t *)&ts_s);
        struct tm *tme = localtime((time_t *)&ts_e);
        strftime(s_buf, sizeof(s_buf), "%Y-%m-%d %H:%M", tms);
        strftime(e_buf, sizeof(e_buf), "%Y-%m-%d %H:%M", tme);

        NshVal vals[4] = {
            val_int((int64_t)sqlite3_column_int64(stmt, 0)),
            val_int((int64_t)sqlite3_column_int  (stmt, 1)),
            val_str(s_buf),
            val_str(e_buf),
        };
        table_append(t, vals);
        for (int i = 0; i < 4; i++) val_free(&vals[i]);
    }
    sqlite3_finalize(stmt);
    return t;
}
