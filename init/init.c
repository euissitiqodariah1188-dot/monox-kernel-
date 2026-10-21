/* MONOX Kernel - Init System (PID 1)
 *
 * Target startup: ≤ 0.33 ms for essential kernel services
 * Services start in parallel using CPU cores.
 * Unit files: .socket, .target, .timer (parsed from /etc/monox/)
 * All service definitions are data-driven; nothing hardcoded.
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "../kernel/sched/sched.h"
#include "../mm/vmm.h"
#include "../fs/vfs/vfs.h"

/* -------------------------------------------------------
 * Unit types
 * -------------------------------------------------------*/
typedef enum {
    UNIT_SERVICE = 0,
    UNIT_SOCKET  = 1,
    UNIT_TARGET  = 2,
    UNIT_TIMER   = 3,
} unit_type_t;

typedef enum {
    STATE_INACTIVE  = 0,
    STATE_ACTIVATING= 1,
    STATE_ACTIVE    = 2,
    STATE_FAILED    = 3,
    STATE_DEACTIVATING=4,
} unit_state_t;

/* -------------------------------------------------------
 * Unit descriptor
 * -------------------------------------------------------*/
#define UNIT_NAME_MAX  64
#define UNIT_DESC_MAX  128
#define UNIT_DEPS_MAX  16
#define UNIT_CMD_MAX   256

struct unit {
    char            name[UNIT_NAME_MAX];
    char            desc[UNIT_DESC_MAX];
    unit_type_t     type;
    unit_state_t    state;

    /* Dependencies (names) */
    char            wants[UNIT_DEPS_MAX][UNIT_NAME_MAX];
    char            after[UNIT_DEPS_MAX][UNIT_NAME_MAX];
    u32             nr_wants;
    u32             nr_after;

    /* Service specific */
    char            exec_start[UNIT_CMD_MAX];
    char            exec_stop[UNIT_CMD_MAX];
    bool            restart_on_failure;
    u32             restart_sec;

    /* Socket specific */
    char            listen_stream[128]; /* unix socket path or port */
    char            listen_datagram[128];

    /* Timer specific */
    u64             on_boot_sec;        /* delay after boot (ns)   */
    u64             on_calendar_ns;     /* periodic interval (ns)  */
    char            timer_unit[UNIT_NAME_MAX]; /* unit to trigger  */

    /* Internal */
    pid_t           pid;
    struct list_head node;
    spinlock_t      lock;
    u64             start_time;
    u64             exit_time;
    s32             exit_code;
};

/* -------------------------------------------------------
 * Global unit list
 * -------------------------------------------------------*/
static LIST_HEAD(g_unit_list);
static spinlock_t g_unit_lock = SPIN_LOCK_INIT;
static u32        g_nr_units  = 0;

static void unit_start_task(struct unit *u);   /* used by unit_exec_wrapper before its definition */

/* helper: does 's' end with 'suffix'? (the old code did ext - 8 without
 * checking the length first and read before the start of the name) */
static bool has_suffix(const char *s, const char *suffix)
{
    size_t sl = strlen(s), xl = strlen(suffix);
    return sl >= xl && strcmp(s + sl - xl, suffix) == 0;
}

/* -------------------------------------------------------
 * unit_new - allocate and init a unit
 * -------------------------------------------------------*/
static struct unit *unit_new(const char *name, unit_type_t type)
{
    struct unit *u = kzalloc(sizeof(*u));
    if (!u) return NULL;

    strncpy(u->name, name ? name : "?", UNIT_NAME_MAX - 1);
    u->type  = type;
    u->state = STATE_INACTIVE;
    spin_lock_init(&u->lock);
    list_init(&u->node);

    spin_lock(&g_unit_lock);
    list_add_tail(&u->node, &g_unit_list);
    g_nr_units++;
    spin_unlock(&g_unit_lock);
    return u;
}

/* -------------------------------------------------------
 * unit_find - find unit by name
 * -------------------------------------------------------*/
static struct unit *unit_find(const char *name)
{
    spin_lock(&g_unit_lock);
    struct list_head *pos;
    list_for_each(pos, &g_unit_list) {
        struct unit *u = list_entry(pos, struct unit, node);
        if (strcmp(u->name, name) == 0) {
            spin_unlock(&g_unit_lock);
            return u;
        }
    }
    spin_unlock(&g_unit_lock);
    return NULL;
}

/* -------------------------------------------------------
 * init_parse_unit_file - parse a .service/.socket/.timer file
 *   path: VFS path like "/etc/monox/networking.service"
 * -------------------------------------------------------*/
static struct unit *init_parse_unit_file(const char *path)
{
    struct vfs_file *f = vfs_open(path, VFS_RDONLY);
    if (!f) {
        pr_warn("INIT: cannot open unit file: %s\n", path);
        return NULL;
    }

    /* Read entire file into buffer */
    u8 buf[2048];
    s64 n = vfs_read(f, buf, sizeof(buf) - 1);
    vfs_close(f);
    if (n <= 0) return NULL;
    buf[n] = '\0';

    /* Determine unit type from extension */
    unit_type_t type = UNIT_SERVICE;
    if (has_suffix(path, ".socket"))       type = UNIT_SOCKET;
    else if (has_suffix(path, ".target"))  type = UNIT_TARGET;
    else if (has_suffix(path, ".timer"))   type = UNIT_TIMER;

    /* Extract basename as unit name */
    const char *basename = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') basename = p + 1;

    struct unit *u = unit_new(basename, type);
    if (!u) return NULL;

    /* Simple key=value parser */
    char *line = (char *)buf;
    while (*line) {
        /* Skip blank lines and comments */
        while (*line == '\n' || *line == '\r' || *line == ' ') line++;
        if (*line == '#' || *line == '[') {
            while (*line && *line != '\n') line++;
            continue;
        }
        if (!*line) break;

        char *eq = line;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') { while (*line && *line != '\n') line++; continue; }

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        char *end = val;
        while (*end && *end != '\n') end++;
        if (*end) { *end = '\0'; line = end + 1; }
        else      { line = end; }     /* stop at the terminator (end+1 read uninitialised stack) */

        /* Strip trailing whitespace */
        for (char *p = key + strlen(key) - 1; p >= key && (*p == ' ' || *p == '\t'); p--) *p = '\0';
        for (char *p = val + strlen(val) - 1; p >= val && (*p == ' ' || *p == '\t'); p--) *p = '\0';

        if (strcmp(key, "Description") == 0)
            strncpy(u->desc, val, UNIT_DESC_MAX - 1);
        else if (strcmp(key, "ExecStart") == 0)
            strncpy(u->exec_start, val, UNIT_CMD_MAX - 1);
        else if (strcmp(key, "ExecStop") == 0)
            strncpy(u->exec_stop, val, UNIT_CMD_MAX - 1);
        else if (strcmp(key, "Restart") == 0)
            u->restart_on_failure = (strcmp(val, "on-failure") == 0 ||
                                     strcmp(val, "always")     == 0);
        else if (strcmp(key, "RestartSec") == 0) {
            u64 sec = 0;
            for (const char *p = val; *p >= '0' && *p <= '9'; p++)
                sec = sec * 10 + (*p - '0');
            u->restart_sec = (u32)sec;
        }
        else if (strcmp(key, "Wants") == 0 && u->nr_wants < UNIT_DEPS_MAX) {
            strncpy(u->wants[u->nr_wants++], val, UNIT_NAME_MAX - 1);
        }
        else if (strcmp(key, "After") == 0 && u->nr_after < UNIT_DEPS_MAX) {
            strncpy(u->after[u->nr_after++], val, UNIT_NAME_MAX - 1);
        }
        else if (strcmp(key, "ListenStream") == 0)
            strncpy(u->listen_stream, val, 127);
        else if (strcmp(key, "OnBootSec") == 0) {
            u64 sec = 0;
            for (const char *p = val; *p >= '0' && *p <= '9'; p++)
                sec = sec * 10 + (*p - '0');
            u->on_boot_sec = sec * 1000000000ULL;
        }
        else if (strcmp(key, "Unit") == 0)
            strncpy(u->timer_unit, val, UNIT_NAME_MAX - 1);
    }

    return u;
}

/* -------------------------------------------------------
 * unit_start_task - run a unit's ExecStart as a kernel task
 * -------------------------------------------------------*/
static void unit_exec_wrapper(void *arg)
{
    struct unit *u = (struct unit *)arg;

    spin_lock(&u->lock);
    u->state      = STATE_ACTIVE;
    spin_unlock(&u->lock);

    pr_info("INIT: unit '%s' started\n", u->name);

    /* In a real OS this would exec() a userspace binary.
     * Here we call a registered service handler by name. */
    extern int service_dispatch(const char *name);
    int ret = service_dispatch(u->exec_start);

    spin_lock(&u->lock);
    u->exit_code = ret;
    u->state     = (ret == 0) ? STATE_INACTIVE : STATE_FAILED;
    spin_unlock(&u->lock);

    if (ret != 0 && u->restart_on_failure) {
        pr_warn("INIT: unit '%s' failed (%d), restarting in %us\n",
                u->name, ret, u->restart_sec);
        task_sleep((u64)u->restart_sec * 1000000000ULL);
        unit_start_task(u);
    }
}

static void unit_start_task(struct unit *u)
{
    spin_lock(&u->lock);
    if (u->state == STATE_ACTIVE || u->state == STATE_ACTIVATING) {
        spin_unlock(&u->lock);
        return;
    }
    u->state = STATE_ACTIVATING;
    spin_unlock(&u->lock);

    task_create(u->name, unit_exec_wrapper, u, PRIO_NORMAL, 32768);
}

/* -------------------------------------------------------
 * init_load_units - scan /etc/monox/ and load all unit files
 * -------------------------------------------------------*/
static void init_load_units(void)
{
    struct vfs_dir *dir = vfs_opendir("/etc/monox");
    if (!dir) {
        pr_warn("INIT: /etc/monox not found, no units to load\n");
        return;
    }

    char path[256];
    struct vfs_dirent de;
    u32 loaded = 0;

    while (vfs_readdir(dir, &de) == MX_OK) {
        bool is_unit = (has_suffix(de.name, ".service") ||
                        has_suffix(de.name, ".socket")  ||
                        has_suffix(de.name, ".target")  ||
                        has_suffix(de.name, ".timer"));
        if (!is_unit) continue;

        strncpy(path, "/etc/monox/", 12);
        strncpy(path + 11, de.name, sizeof(path) - 12);
        path[sizeof(path) - 1] = '\0';          /* a 244+ char name was not terminated */

        struct unit *u = init_parse_unit_file(path);
        if (u) loaded++;
    }

    vfs_closedir(dir);
    pr_info("INIT: loaded %u unit files from /etc/monox/\n", loaded);
}

/* -------------------------------------------------------
 * init_start_default_target - start all units in parallel
 * -------------------------------------------------------*/
static void init_start_default_target(void)
{
    /* Start all units with no 'After' dependencies in parallel,
     * then units with dependencies after their deps complete.
     * Simple two-pass for now; a full dep-solver can be added. */

    struct list_head *pos;

    /* Pass 1: units with no after-deps */
    list_for_each(pos, &g_unit_list) {
        struct unit *u = list_entry(pos, struct unit, node);
        if (u->nr_after == 0 && u->type == UNIT_SERVICE &&
            strlen(u->exec_start) > 0) {
            unit_start_task(u);
        }
    }

    /* Pass 2: units with deps (wait for deps to be ACTIVE) */
    list_for_each(pos, &g_unit_list) {
        struct unit *u = list_entry(pos, struct unit, node);
        if (u->nr_after == 0) continue;
        if (u->type != UNIT_SERVICE) continue;
        if (!strlen(u->exec_start)) continue;

        /* Wait until all 'after' deps are active */
        bool ready = false;
        u32 attempts = 1000;
        while (!ready && attempts--) {
            ready = true;
            for (u32 i = 0; i < u->nr_after; i++) {
                struct unit *dep = unit_find(u->after[i]);
                if (!dep || dep->state != STATE_ACTIVE) {
                    ready = false;
                    break;
                }
            }
            if (!ready) schedule();
        }
        unit_start_task(u);
    }
}

/* -------------------------------------------------------
 * PID 1 entry point
 * -------------------------------------------------------*/
void __noreturn monox_init(void)
{
    pr_info("INIT: PID 1 starting (MONOX init system)\n");

    /* Mount root filesystem */
    extern int vfs_mount_root(void);
    int mr = vfs_mount_root();
    if (mr != MX_OK) {
        /* No block driver / no MonoxFS volume: this used to panic the whole
         * kernel, killing every subsystem that was working. Stay up in a
         * degraded mode instead (no units, maintenance loop only). */
        pr_err("INIT: cannot mount root filesystem (%d): running without units\n", mr);
    } else {
        /* Load unit files from /etc/monox/ */
        init_load_units();

        /* Start default target */
        init_start_default_target();
    }

    pr_info("INIT: %u units loaded, init complete\n", g_nr_units);

    /* PID 1 main loop: reap zombies, restart failed services */
    for (;;) {
        struct list_head *pos;
        list_for_each(pos, &g_unit_list) {
            struct unit *u = list_entry(pos, struct unit, node);
            spin_lock(&u->lock);
            unit_state_t st = u->state;
            spin_unlock(&u->lock);

            if (st == STATE_FAILED && u->restart_on_failure) {
                task_sleep((u64)u->restart_sec * 1000000000ULL);
                unit_start_task(u);
            }
        }
        /* Yield for ~1 second */
        task_sleep(1000000000ULL);
    }
}
