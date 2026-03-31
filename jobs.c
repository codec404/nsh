#include "shell.h"

static Job job_table[MAX_JOBS];
static int njobs = 0;

void jobs_init(void)
{
    memset(job_table, 0, sizeof(job_table));
    njobs = 0;
}

int jobs_count(void) { return njobs; }

/* ── allocate next job number ───────────────────────────────────────────── */

static int next_job_num(void)
{
    /* find smallest positive integer not already in use */
    for (int n = 1; n <= MAX_JOBS; n++) {
        int used = 0;
        for (int i = 0; i < njobs; i++)
            if (job_table[i].num == n) { used = 1; break; }
        if (!used) return n;
    }
    return -1;
}

/* ── add ────────────────────────────────────────────────────────────────── */

Job *jobs_add(pid_t pgid, pid_t *pids, int npids, const char *cmdline)
{
    if (njobs >= MAX_JOBS) {
        fprintf(stderr, "nsh: too many background jobs\n");
        return NULL;
    }

    Job *j = &job_table[njobs++];
    j->num      = next_job_num();
    j->pgid     = pgid;
    j->npids    = npids;
    j->ndone    = 0;
    j->state    = JOB_RUNNING;
    j->notified = 0;
    j->cmdline  = cmdline ? strdup(cmdline) : NULL;

    j->pids = malloc(npids * sizeof(pid_t));
    if (j->pids)
        memcpy(j->pids, pids, npids * sizeof(pid_t));

    return j;
}

/* ── remove a finished job from the table ───────────────────────────────── */

static void jobs_remove(Job *j)
{
    free(j->pids);
    free(j->cmdline);
    /* compact the array */
    int idx = (int)(j - job_table);
    memmove(&job_table[idx], &job_table[idx + 1],
            (njobs - idx - 1) * sizeof(Job));
    njobs--;
}

/* ── state update ───────────────────────────────────────────────────────── */

void jobs_set_state(Job *j, JobState state)
{
    j->state = state;
}

/* ── lookups ────────────────────────────────────────────────────────────── */

Job *jobs_find_num(int num)
{
    for (int i = 0; i < njobs; i++)
        if (job_table[i].num == num)
            return &job_table[i];
    return NULL;
}

Job *jobs_find_pgid(pid_t pgid)
{
    for (int i = 0; i < njobs; i++)
        if (job_table[i].pgid == pgid)
            return &job_table[i];
    return NULL;
}

/*
 * jobs_last — return the job most recently added or most recently stopped,
 * in that priority order.  Used when `fg` or `bg` is called with no argument.
 */
Job *jobs_last(void)
{
    /* prefer a stopped job (most recently added to table that is stopped) */
    for (int i = njobs - 1; i >= 0; i--)
        if (job_table[i].state == JOB_STOPPED)
            return &job_table[i];
    /* fall back to most recently added running job */
    if (njobs > 0)
        return &job_table[njobs - 1];
    return NULL;
}

/* ── display ────────────────────────────────────────────────────────────── */

static const char *state_str(JobState s)
{
    switch (s) {
    case JOB_RUNNING: return "Running";
    case JOB_STOPPED: return "Stopped";
    case JOB_DONE:    return "Done";
    }
    return "?";
}

void jobs_print_all(void)
{
    for (int i = 0; i < njobs; i++) {
        Job *j = &job_table[i];
        printf("[%d]%s  %-10s %s\n",
               j->num,
               (i == njobs - 1) ? "+" : (i == njobs - 2) ? "-" : " ",
               state_str(j->state),
               j->cmdline ? j->cmdline : "");
    }
}

/* ── reap_jobs ──────────────────────────────────────────────────────────── */

/*
 * Called from the main REPL before each prompt (never from a signal handler).
 *
 * Drains all pending child state changes with WNOHANG so we don't block,
 * updates job states, and prints notifications for completed/stopped jobs.
 * Removes done jobs that have been notified.
 */
void reap_jobs(void)
{
    int wstatus;
    pid_t pid;

    /*
     * Loop until no more children have pending state changes.
     * WUNTRACED lets us see stopped children too.
     */
    while ((pid = waitpid(-1, &wstatus, WNOHANG | WUNTRACED)) > 0) {
        /* find which job this pid belongs to */
        Job *j = NULL;
        for (int i = 0; i < njobs; i++) {
            for (int k = 0; k < job_table[i].npids; k++) {
                if (job_table[i].pids[k] == pid) {
                    j = &job_table[i];
                    break;
                }
            }
            if (j) break;
        }
        if (!j) continue;   /* pid from a foreground job already waited */

        if (WIFSTOPPED(wstatus)) {
            j->ndone++;     /* count stopped as "done waiting" for this pid */
            if (j->ndone == j->npids) {
                jobs_set_state(j, JOB_STOPPED);
                fprintf(stderr, "\n[%d]+  Stopped\t\t%s\n",
                        j->num, j->cmdline ? j->cmdline : "");
                j->notified = 1;
                j->ndone = 0;   /* reset so fg can wait again */
            }
        } else {
            j->ndone++;
            if (j->ndone == j->npids) {
                jobs_set_state(j, JOB_DONE);
            }
        }
    }

    /* print and purge done jobs */
    for (int i = njobs - 1; i >= 0; i--) {
        Job *j = &job_table[i];
        if (j->state == JOB_DONE && !j->notified) {
            fprintf(stderr, "[%d]+  Done\t\t%s\n",
                    j->num, j->cmdline ? j->cmdline : "");
            j->notified = 1;
        }
        if (j->state == JOB_DONE && j->notified)
            jobs_remove(j);
    }
}
