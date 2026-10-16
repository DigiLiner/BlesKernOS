#include "../include/admin.h"
#include "../include/api.h"
#include "../include/compat_mode.h"
#include "../include/klock.h"
#include "../include/pit.h"
#include "../include/smp.h"
#include "../include/usercopy.h"
#include "../include/wine_policy.h"
#define ADMIN_CONFIG "/SYSTEM/USER/CONFIG/STARTUP.INI"
#define EVENT_LOG "/SYSTEM/LOGS/EVENTS.LOG"
static kspinlock_t lock = KSPINLOCK_INITIALIZER;
static kmutex_t config_lock = KMUTEX_INITIALIZER;
static bk_event_info_t events[BK_EVENT_CAPACITY];
static bk_startup_info_t entries[BK_ADMIN_MAX];
static uint32_t event_seq, event_used, entry_count;
static bool initialized, storage, desktop_ready, export_pending, runtime_dirty;
static uint32_t desktop_tick;
static char lines[SMP_MAX_CPUS][192];
static uint32_t line_used[SMP_MAX_CPUS];
static uint32_t commands[BK_ADMIN_MAX], started_at[BK_ADMIN_MAX];
static bool restart_pending[BK_ADMIN_MAX], launched[BK_ADMIN_MAX];

static char log_buffer[BK_EVENT_CAPACITY * 320U], config_buffer[12288];
static uint32_t ms(void) {
    uint32_t hz = pit_get_frequency_hz();
    return hz ? (uint32_t)((uint64_t)pit_get_ticks() * 1000U / hz) : 0;
}
static void text(char *dst, uint32_t n, const char *src) {
    kstrncpy(dst, src ? src : "", n - 1U);
    dst[n - 1U] = 0;
}
static bool has(const char *s, const char *needle) {
    for (; *s; s++)
        if (!kstrncmp(s, needle, kstrlen(needle)))
            return true;
    return false;
}
void admin_emit(uint32_t source, uint32_t severity, const char *code, const char *message,
                uint32_t pid) {
    if (!initialized)
        return;
    uint32_t flags;
    /* Producers may run inside IRQs or existing subsystem locks. Never wait. */
    if (!kspin_try_lock_irqsave(&lock, &flags))
        return;
    bk_event_info_t *e = &events[event_seq % BK_EVENT_CAPACITY];
    kmemset(e, 0, sizeof(*e));
    e->sequence = ++event_seq;
    e->uptime_ms = ms();
    e->pid = pid;
    e->source = source;
    e->severity = severity;
    text(e->code, sizeof(e->code), code);
    text(e->message, sizeof(e->message), message);
    if (event_used < BK_EVENT_CAPACITY)
        event_used++;
    kspin_unlock_irqrestore(&lock, flags);
}
void admin_capture_char(char c) {
    if (!initialized)
        return;
    uint32_t cpu = smp_cpu_index();
    if (cpu >= SMP_MAX_CPUS)
        cpu = 0;
    /* Same-CPU interrupts may interrupt console output: protect line assembly,
     * but release before publishing the completed record. */
    uint32_t f;
    if (!kspin_try_lock_irqsave(&lock, &f))
        return;
    char completed[192];
    bool ready = false;
    if (c == '\n') {
        lines[cpu][line_used[cpu]] = 0;
        text(completed, sizeof(completed), lines[cpu]);
        line_used[cpu] = 0;
        ready = completed[0] == '[';
    } else if (c != '\r' && line_used[cpu] < 191U)
        lines[cpu][line_used[cpu]++] = c;
    kspin_unlock_irqrestore(&lock, f);
    if (!ready)
        return;
    uint32_t src = BK_EVENT_KERNEL, sev = BK_EVENT_INFO;
    if (has(completed, "[MM") || has(completed, "PAGING"))
        src = BK_EVENT_MEMORY;
    else if (has(completed, "DVR") || has(completed, "DRIVER"))
        src = BK_EVENT_DRIVER;
    else if (has(completed, "VFS") || has(completed, "BFS") || has(completed, "FAT") ||
             has(completed, "ATAPI"))
        src = BK_EVENT_DISK;
    else if (has(completed, "NET") || has(completed, "DHCP") || has(completed, "TLS"))
        src = BK_EVENT_NETWORK;
    else if (has(completed, "WINE") || has(completed, "VISION") || has(completed, "PRINT"))
        src = BK_EVENT_SERVICE;
    else if (has(completed, "ELF") || has(completed, "TASK") || has(completed, "API"))
        src = BK_EVENT_APPLICATION;
    if (has(completed, "ERROR") || has(completed, "PANIC") || has(completed, "failed") ||
        has(completed, "fallo"))
        sev = BK_EVENT_ERROR;
    else if (has(completed, "aviso") || has(completed, "timeout") || has(completed, "inversion"))
        sev = BK_EVENT_WARNING;
    admin_emit(src, sev, "CONSOLE", completed, task_current_process_id());
}
static void builtin(uint32_t i, const char *name, const char *description, const char *path,
                    bool critical) {
    bk_startup_info_t *e = &entries[i];
    e->id = i + 1;
    e->enabled = 1;
    e->service = 1;
    e->critical = critical;
    e->timeout_ms = 30000;
    e->order = i;
    text(e->name, sizeof(e->name), name);
    text(e->description, sizeof(e->description), description);
    text(e->path, sizeof(e->path), path);
}
void admin_init(void) {
    builtin(0, "Vision", "Motor grafico y escritorio", "/SYSTEM/SERVICES/VISION.BEX", true);
    builtin(1, "PrintSpool", "Cola de impresion", "/SYSTEM/SERVICES/PRINTSPL.BEX", false);
    builtin(2, "Wine", "Entorno de aplicaciones Windows", "/WINEGATE/bin/wine-launch.ELF", true);
    /* Service registry entry remains visible, but an old SERVICES.INI
     * setting must not silently warm up Wine during desktop startup. The
     * real Wine launcher is called by the user/application on demand. */
    entries[2].enabled = false;
    entries[2].phase = BK_PHASE_DESKTOP;
    entries[2].dependencies = 1;
    entries[2].timeout_ms = 180000;
    entries[2].arguments[0] = '\0';
    launched[2] = true;
    entry_count = 3;
    initialized = true;
    admin_emit(BK_EVENT_KERNEL, BK_EVENT_INFO, "ADMIN_INIT",
               "Registro de eventos y servicios inicializado", 0);
}
bool admin_enabled(uint32_t id) {
    if (!id || id > entry_count)
        return false;
    return entries[id - 1].enabled != 0;
}
void admin_adopt(uint32_t id, uint32_t pid) {
    if (!id || id > entry_count)
        return;
    uint32_t f = kspin_lock_irqsave(&lock);
    entries[id - 1].pid = pid;
    entries[id - 1].state = BK_SERVICE_STARTING;
    started_at[id - 1] = ms();
    launched[id - 1] = true;
    runtime_dirty = true;
    kspin_unlock_irqrestore(&lock, f);
    admin_emit(BK_EVENT_SERVICE, 0, "SERVICE_START", entries[id - 1].name, pid);
}
void admin_ready(uint32_t id) {
    if (!id || id > entry_count) return;
    uint32_t f = kspin_lock_irqsave(&lock);
    uint32_t pid = entries[id - 1].pid;
    bool late = entries[id - 1].state == BK_SERVICE_FAILED &&
        !kstrcmp(entries[id - 1].last_error, "SERVICE_START_TIMEOUT");
    kspin_unlock_irqrestore(&lock, f);
    /* Slow USB 1.1 root I/O can delay an otherwise healthy Vision. A timeout
     * is still reported, but explicit readiness from its live process must
     * unblock dependents. Never revive a dead/replaced process or other errors.
     * Query scheduler state outside the admin spinlock to preserve lock order. */
    bool alive = false;
    if (late && pid) task_query_process(pid, &alive, NULL, NULL);
    f = kspin_lock_irqsave(&lock);
    bool changed = entries[id - 1].pid == pid &&
        (entries[id - 1].state == BK_SERVICE_STARTING ||
         (late && alive && entries[id - 1].state == BK_SERVICE_FAILED &&
          !kstrcmp(entries[id - 1].last_error, "SERVICE_START_TIMEOUT")));
    if (changed) {
        entries[id - 1].state = BK_SERVICE_RUNNING;
        entries[id - 1].last_result = 0;
        entries[id - 1].last_error[0] = 0;
        runtime_dirty = true;
    }
    kspin_unlock_irqrestore(&lock, f);
    if (changed) admin_emit(BK_EVENT_SERVICE, 0, "SERVICE_READY", entries[id - 1].name, pid);
}
void admin_fail(uint32_t id, const char *reason) {
    if (!id || id > entry_count)
        return;
    uint32_t f = kspin_lock_irqsave(&lock);
    entries[id - 1].state = BK_SERVICE_FAILED;
    entries[id - 1].last_result = 1;
    runtime_dirty = true;
    text(entries[id - 1].last_error, 96, reason);
    uint32_t pid = entries[id - 1].pid;
    kspin_unlock_irqrestore(&lock, f);
    admin_emit(BK_EVENT_SERVICE, 2, reason, entries[id - 1].name, pid);
}
uint32_t bk_event_count(void) { return event_used; }
bool bk_event_get(uint32_t index, bk_event_info_t *out) {
    bk_event_info_t e;
    uint32_t f = kspin_lock_irqsave(&lock);
    bool ok = index < event_used;
    if (ok)
        e = events[(event_seq - 1 - index) % BK_EVENT_CAPACITY];
    kspin_unlock_irqrestore(&lock, f);
    return ok && copy_to_user(out, &e, sizeof(e));
}
bool bk_event_export(void) {
    if (!storage)
        return false;
    uint32_t f = kspin_lock_irqsave(&lock);
    export_pending = true;
    kspin_unlock_irqrestore(&lock, f);
    return true;
}
uint32_t bk_startup_count(void) { return entry_count; }
bool bk_startup_get(uint32_t index, bk_startup_info_t *out) {
    bk_startup_info_t e;
    uint32_t f = kspin_lock_irqsave(&lock);
    bool ok = index < entry_count;
    if (ok)
        e = entries[index];
    kspin_unlock_irqrestore(&lock, f);
    return ok && copy_to_user(out, &e, sizeof(e));
}
static bool persist(void) {
    uint32_t used = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        bk_startup_info_t e;
        uint32_t f = kspin_lock_irqsave(&lock);
        e = entries[i];
        kspin_unlock_irqrestore(&lock, f);
        int n = snprintf(config_buffer + used, sizeof(config_buffer) - used,
                         "%u|%u|%u|%u|%u|%u|%u|%s|%s|%s|%s|%u|%s\n", e.id, e.enabled, e.phase,
                         e.order, e.service, e.dependencies, e.timeout_ms, e.name, e.path,
                         e.arguments, e.description, e.last_result, e.last_error);
        if (n < 0 || (uint32_t)n >= sizeof(config_buffer) - used)
            return false;
        used += n;
    }
    if (!vfs_write_all(ADMIN_CONFIG ".NEW", config_buffer, used))
        return false;
    return vfs_rename(ADMIN_CONFIG ".NEW", ADMIN_CONFIG);
}
static bool clean(const char *s) {
    for (; *s; s++)
        if (*s == '|' || *s == '\n' || *s == '\r')
            return false;
    return true;
}
static bool valid(bk_startup_info_t *e) {
    e->name[31] = e->description[79] = e->path[159] = e->arguments[159] = 0;
    if (!e->name[0] || e->phase > 2 || e->order > 999 || e->timeout_ms < 1000 ||
        e->timeout_ms > 180000 || e->dependencies & ~((1U << BK_ADMIN_MAX) - 1U))
        return false;
    if (!clean(e->name) || !clean(e->description) || !clean(e->path) || !clean(e->arguments))
        return false;
    if ((!e->id || e->id > 3) && e->path[0] != '/')
        return false;
    return true;
}
/* Resolve transitive dependencies before committing a definition. IDs are stable
 * bit positions; deleted slots remain tombstones in the persistent file. */
static bool dependencies_valid(const bk_startup_info_t *candidate, uint32_t index) {
    uint32_t pending = candidate->dependencies, seen = 0;
    while (pending) {
        uint32_t d = 0;
        while (!(pending & (1U << d)))
            d++;
        pending &= ~(1U << d);
        if (d == index || d >= entry_count || !entries[d].name[0] ||
            entries[d].phase > candidate->phase)
            return false;
        if (!(seen & (1U << d))) {
            seen |= 1U << d;
            pending |= entries[d].dependencies & ~seen;
        }
    }
    return true;
}
bool bk_startup_save(const bk_startup_info_t *in) {
    bk_startup_info_t e;
    if (!copy_from_user(&e, in, sizeof(e)) || !valid(&e) || !storage)
        return false;
    bool requested_enabled = e.enabled != 0;
    kmutex_lock(&config_lock);
    uint32_t f = kspin_lock_irqsave(&lock);
    uint32_t index = e.id ? e.id - 1 : entry_count;
    if (!e.id)
        for (uint32_t i = 3; i < entry_count; i++)
            if (!entries[i].name[0]) {
                index = i;
                break;
            }
    bool ok = index < BK_ADMIN_MAX && index <= entry_count;
    if (ok && index >= 3)
        ok = dependencies_valid(&e, index) &&
             (!entries[index].pid ||
              (e.phase == entries[index].phase && e.service == entries[index].service &&
               e.dependencies == entries[index].dependencies &&
               !kstrcmp(e.path, entries[index].path) &&
               !kstrcmp(e.arguments, entries[index].arguments)));
    bk_startup_info_t old;
    if (ok) {
        old = entries[index];
        if (index < 3) {
            e = old;
            e.enabled = requested_enabled ? 1 : 0;
            if (index == 0)
                e.enabled = 1;
        } else {
            e.enabled = !!e.enabled;
            e.service = !!e.service;
            e.critical = 0;
            e.pid = old.name[0] ? old.pid : 0;
            e.state = old.name[0] ? old.state : BK_SERVICE_STOPPED;
            e.last_result = old.name[0] ? old.last_result : 0;
            text(e.last_error, 96, old.name[0] ? old.last_error : "");
        }
        e.id = index + 1;
        entries[index] = e;
        if (index == entry_count)
            entry_count++;
    }
    kspin_unlock_irqrestore(&lock, f);
    if (ok && !persist()) {
        f = kspin_lock_irqsave(&lock);
        entries[index] = old;
        if (!old.id)
            entry_count--;
        kspin_unlock_irqrestore(&lock, f);
        ok = false;
    }
    if (ok && index >= 3)
        launched[index] = true; /* Definition edits take effect next boot. */
    kmutex_unlock(&config_lock);
    return ok;
}
bool bk_startup_remove(uint32_t id) {
    if (id <= 3 || id > entry_count)
        return false;
    kmutex_lock(&config_lock);
    uint32_t f = kspin_lock_irqsave(&lock);
    bool ok = entries[id - 1].pid == 0 && !commands[id - 1];
    for (uint32_t d = 0; d < entry_count; d++)
        if (entries[d].dependencies & (1U << (id - 1)))
            ok = false;
    bk_startup_info_t old = entries[id - 1];
    if (ok) {
        entries[id - 1].enabled = 0;
        entries[id - 1].path[0] = 0;
        entries[id - 1].name[0] = 0;
        entries[id - 1].dependencies = 0;
        launched[id - 1] = true;
        restart_pending[id - 1] = false;
    }
    kspin_unlock_irqrestore(&lock, f);
    if (ok && !persist()) {
        f = kspin_lock_irqsave(&lock);
        entries[id - 1] = old;
        kspin_unlock_irqrestore(&lock, f);
        ok = false;
    }
    kmutex_unlock(&config_lock);
    return ok;
}
bool bk_service_control(uint32_t id, uint32_t action) {
    if (!id || id > entry_count || action < 1 || action > 3)
        return false;
    /* The boot supervisor owns Vision's trusted loader and restart lifecycle.
     * Keep stop protection, but permit an explicit desktop-server restart. */
    if(id==1U && action==BK_SERVICE_RESTART) {
        uint32_t owner=gui_vision_owner_pid();
        return owner && task_request_exit_process(owner,0);
    }
    uint32_t f = kspin_lock_irqsave(&lock);
    bk_startup_info_t *e = &entries[id - 1];
    bool ok = e->name[0] && !e->critical && !commands[id - 1] && e->state != BK_SERVICE_STARTING &&
              e->state != BK_SERVICE_STOPPING;
    if (action == BK_SERVICE_START && e->pid)
        ok = false;
    if (action != BK_SERVICE_START && !e->pid)
        ok = false;
    if (action != BK_SERVICE_START)
        for (uint32_t d = 0; d < entry_count; d++)
            if (entries[d].pid && (entries[d].dependencies & (1U << (id - 1))))
                ok = false;
    if (ok)
        commands[id - 1] = action;
    kspin_unlock_irqrestore(&lock, f);
    return ok;
}
bool bk_service_ready(uint32_t id) {
    if (!id)
        return false;
    uint32_t f = kspin_lock_irqsave(&lock);
    bool ok = id <= entry_count && entries[id - 1].service &&
              entries[id - 1].pid == task_current_process_id() &&
              (entries[id - 1].state == BK_SERVICE_STARTING ||
               entries[id - 1].state == BK_SERVICE_RUNNING);
    kspin_unlock_irqrestore(&lock, f);
    if (ok)
        admin_ready(id);
    return ok;
}
bool bk_memory_detail(bk_memory_detail_t *out) {
    system_memory_info_t m;
    heap_info_t h;
    mm_get_system_info(&m);
    mm_get_info(&h);
    bk_memory_detail_t d = {m.total_bytes,
                            m.used_bytes,
                            m.free_bytes,
                            m.reserved_bytes,
                            h.total_bytes,
                            h.used_bytes,
                            h.free_bytes,
                            mm_vm_frame_total_pages() * 4096U,
                            mm_vm_frame_free_pages() * 4096U};
    return copy_to_user(out, &d, sizeof(d));
}
static bool ipv4(const char *s, uint8_t out[4]) {
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t n = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s++ - '0');
            if (++digits > 3 || n > 255)
                return false;
        }
        if (!digits)
            return false;
        out[i] = n;
        if (i < 3) {
            if (*s++ != '.')
                return false;
        } else if (*s)
            return false;
    }
    return true;
}
bool bk_network_profile_apply(uint32_t profile, bool dhcp, const char *ip, const char *mask,
                              const char *gateway, const char *dns) {
    if (profile > 2U)
        return false;
    char a[4][32];
    const char *p[] = {ip, mask, gateway, dns};
    uint8_t bytes[4][4];
    for (unsigned i = 0; i < 4; i++) {
        if (!copy_string_from_user(a[i], 32, p[i]) || !ipv4(a[i], bytes[i]))
            return false;
    }
    /* Reject non-contiguous masks. */
    uint32_t m = ((uint32_t)bytes[1][0] << 24) | ((uint32_t)bytes[1][1] << 16) |
                 ((uint32_t)bytes[1][2] << 8) | bytes[1][3];
    uint32_t inv = ~m;
    if (!dhcp && (!m || (inv & (inv + 1U))))
        return false;
    bool ok = dhcp ? network_dhcp(5000) : network_configure(bytes[0], bytes[1], bytes[2], bytes[3]);
    if (ok) {
        char data[256];
        int n = snprintf(data, sizeof(data), "dhcp=%u\nip=%s\nmask=%s\ngateway=%s\ndns=%s\n",
                         !!dhcp, a[0], a[1], a[2], a[3]);
        char profile_path[96];
        snprintf(profile_path, sizeof(profile_path), "/SYSTEM/USER/CONFIG/NETWORK%u.INI",
                 profile + 1);
        char temporary[100];
        snprintf(temporary, sizeof(temporary), "%s.NEW", profile_path);
        kmutex_lock(&config_lock);
        ok = vfs_write_all(temporary, data, n) && vfs_rename(temporary, profile_path) &&
             vfs_write_all("/SYSTEM/USER/CONFIG/NETWORK.INI.NEW", data, n) &&
             vfs_rename("/SYSTEM/USER/CONFIG/NETWORK.INI.NEW", "/SYSTEM/USER/CONFIG/NETWORK.INI");
        kmutex_unlock(&config_lock);
    }
    admin_emit(BK_EVENT_NETWORK, ok ? 0 : 2, ok ? "PROFILE_APPLIED" : "PROFILE_FAILED",
               ok ? "Perfil aplicado y guardado" : "No se pudo aplicar o guardar el perfil",
               task_current_process_id());
    return ok;
}
static void load_config(void) {
    void *raw = NULL;
    uint32_t size = 0;
    if (!vfs_read_all(ADMIN_CONFIG, &raw, &size) || !raw)
        return;
    if (size > sizeof(config_buffer)) {
        kfree(raw);
        return;
    }
    char *s = raw;
    uint32_t pos = 0;
    while (pos < size) {
        char line[768];
        uint32_t n = 0;
        while (pos < size && s[pos] != '\n') {
            if (n + 1 < sizeof(line) && s[pos] != '\r')
                line[n++] = s[pos];
            pos++;
        }
        pos++;
        line[n] = 0;
        char *part[13];
        unsigned count = 1;
        part[0] = line;
        for (char *p = line; *p; p++)
            if (*p == '|' && count < 13) {
                *p = 0;
                part[count++] = p + 1;
            }
        if (count != 11 && count != 13)
            continue;
        bk_startup_info_t e;
        kmemset(&e, 0, sizeof(e));
        unsigned *nums[] = {&e.id,      &e.enabled,      &e.phase,     &e.order,
                            &e.service, &e.dependencies, &e.timeout_ms};
        bool good = true;
        for (unsigned i = 0; i < 7; i++) {
            uint32_t value = 0;
            for (char *p = part[i]; *p; p++) {
                if (*p < '0' || *p > '9' || value > 180000) {
                    good = false;
                    break;
                }
                value = value * 10 + *p - '0';
            }
            *nums[i] = value;
        }
        if (!good || !e.id || e.id > BK_ADMIN_MAX)
            continue;
        text(e.name, 32, part[7]);
        text(e.path, 160, part[8]);
        text(e.arguments, 160, part[9]);
        text(e.description, 80, part[10]);
        if (count == 13) {
            for (char *p = part[11]; *p; p++) {
                if (*p < '0' || *p > '9') {
                    good = false;
                    break;
                }
                e.last_result = e.last_result * 10U + (uint32_t)(*p - '0');
            }
            text(e.last_error, 96, part[12]);
            if (!good || !clean(e.last_error))
                continue;
        }
        if (e.id <= 3) {
            entries[e.id - 1].last_result = e.last_result;
            text(entries[e.id - 1].last_error, 96, e.last_error);
            if (e.id == 2)
                entries[e.id - 1].enabled = !!e.enabled;
        } else if (e.id == entry_count + 1) {
            bool tombstone = !e.name[0] && !e.path[0] && !e.enabled;
            if (tombstone || valid(&e))
                entries[entry_count++] = e;
        }
    }
    kfree(raw);
    for (uint32_t i = 3; i < entry_count; i++) {
        if (entries[i].name[0] && !dependencies_valid(&entries[i], i)) {
            entries[i].enabled = 0;
            admin_fail(i + 1, "SERVICE_INVALID_DEPENDENCIES");
        }
    }
}
static void save_events(bool exported) {
    uint32_t used = 0, last = 0;
    for (uint32_t i = 0; i < event_used; i++) {
        bk_event_info_t e;
        uint32_t f = kspin_lock_irqsave(&lock);
        e = events[(event_seq - event_used + i) % BK_EVENT_CAPACITY];
        kspin_unlock_irqrestore(&lock, f);
        int n =
            snprintf(log_buffer + used, sizeof(log_buffer) - used, "%u\t%u\t%u\t%u\t%u\t%s\t%s\n",
                     e.sequence, e.uptime_ms, e.source, e.severity, e.pid, e.code, e.message);
        if (n < 0 || (uint32_t)n >= sizeof(log_buffer) - used)
            break;
        used += n;
        last = e.sequence;
    }
    static uint32_t saved;
    if (!exported && last == saved)
        return;
    const char *path = exported ? "/SYSTEM/LOGS/EVENTS-EXPORT.TXT" : EVENT_LOG;
    bool ok = false;
    if (vfs_write_all("/SYSTEM/LOGS/EVENTS.NEW", log_buffer, used)) {
        if (!exported)
            vfs_rename(EVENT_LOG, EVENT_LOG ".1");
        ok = vfs_rename("/SYSTEM/LOGS/EVENTS.NEW", path);
        if (ok && !exported)
            saved = last;
    }
    if (exported)
        admin_emit(BK_EVENT_KERNEL, ok ? BK_EVENT_INFO : BK_EVENT_ERROR,
                   ok ? "EVENT_EXPORT_READY" : "EVENT_EXPORT_FAILED", path, 0);
}
static void worker(void *unused UNUSED) {
    uint32_t last_save = 0;
    for (;;) {
        uint32_t now = ms();
        kmutex_lock(&config_lock);
        uint32_t order[BK_ADMIN_MAX], count = entry_count;
        for (uint32_t i = 0; i < count; i++)
            order[i] = i;
        for (uint32_t i = 1; i < count; i++) {
            uint32_t value = order[i], j = i;
            while (j && entries[order[j - 1]].order > entries[value].order) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = value;
        }
        for (uint32_t slot = 0; slot < count; slot++) {
            uint32_t i = order[slot];
            bk_startup_info_t e;
            uint32_t f = kspin_lock_irqsave(&lock);
            e = entries[i];
            uint32_t cmd = commands[i];
            commands[i] = 0;
            kspin_unlock_irqrestore(&lock, f);
            bool alive = false;
            int32_t exit_code = 0;
            if (e.pid)
                task_query_process(e.pid, &alive, &exit_code, NULL);
            if (i == 1 && e.state == BK_SERVICE_STARTING && bk_print_spooler_is_ready())
                admin_ready(2);
            if (e.pid && !alive) {
                f = kspin_lock_irqsave(&lock);
                if(entries[i].pid!=e.pid) {
                    kspin_unlock_irqrestore(&lock,f);
                    continue; /* another supervisor already adopted a replacement */
                }
                entries[i].pid = 0;
                entries[i].last_result = (uint32_t)exit_code;
                entries[i].state =
                    (e.state == BK_SERVICE_STOPPING || (!e.service && exit_code == 0))
                        ? BK_SERVICE_STOPPED
                        : BK_SERVICE_FAILED;
                if (entries[i].state == BK_SERVICE_FAILED)
                    text(entries[i].last_error, 96, "SERVICE_UNEXPECTED_EXIT");
                runtime_dirty = true;
                kspin_unlock_irqrestore(&lock, f);
                admin_emit(5, e.state == 3 ? 0 : 1, "SERVICE_EXIT", e.name, e.pid);
            }
            if (cmd == BK_SERVICE_STOP || cmd == BK_SERVICE_RESTART) {
                if (task_request_exit_process(e.pid, 0)) {
                    f = kspin_lock_irqsave(&lock);
                    entries[i].state = BK_SERVICE_STOPPING;
                    started_at[i] = now;
                    restart_pending[i] = cmd == 3;
                    kspin_unlock_irqrestore(&lock, f);
                    admin_emit(5, 0, "SERVICE_STOP_REQUEST", e.name, e.pid);
                } else
                    admin_fail(i + 1, "SERVICE_STOP_REJECTED");
            }
            f = kspin_lock_irqsave(&lock);
            e = entries[i];
            kspin_unlock_irqrestore(&lock, f);
            if (alive && (e.state == BK_SERVICE_STARTING || e.state == BK_SERVICE_STOPPING) &&
                now - started_at[i] > e.timeout_ms)
                admin_fail(i + 1, e.state == 3 ? "SERVICE_STOP_TIMEOUT" : "SERVICE_START_TIMEOUT");
            if (i < 3 && cmd != BK_SERVICE_START && !restart_pending[i])
                continue;
            bool due =
                e.phase == 0 || (desktop_ready && (e.phase == 1 || now - desktop_tick >= 5000));
            bool start =
                cmd == 1 || (!e.pid && restart_pending[i]) || (!launched[i] && e.enabled && due);
            if (!start || e.pid || !e.path[0])
                continue;
            bool dependencies = true;
            for (uint32_t d = 0; d < entry_count; d++)
                if ((e.dependencies & (1U << d)) && entries[d].state != BK_SERVICE_RUNNING)
                    dependencies = false;
            if (!dependencies) {
                if (cmd == BK_SERVICE_START)
                    admin_emit(BK_EVENT_SERVICE, BK_EVENT_WARNING, "SERVICE_DEPENDENCY_WAIT",
                               e.name, 0);
                continue;
            }
            launched[i] = true;
            restart_pending[i] = false;
            if (i == 1)
                bk_print_spooler_set_ready(false);
            int pid = elf_spawn_program_ex(e.path, gui_get_desktop(), e.arguments);
            if (pid < 0)
                admin_fail(i + 1, "SERVICE_SPAWN_FAILED");
            else {
                admin_adopt(i + 1, pid);
                if (!e.service)
                    admin_ready(i + 1);
            }
        }
        if (storage && runtime_dirty && now - last_save > 5000) {
            uint32_t f = kspin_lock_irqsave(&lock);
            runtime_dirty = false;
            kspin_unlock_irqrestore(&lock, f);
            if (!persist()) {
                f = kspin_lock_irqsave(&lock);
                runtime_dirty = true;
                kspin_unlock_irqrestore(&lock, f);
            }
        }
        kmutex_unlock(&config_lock);
        if (storage && (now - last_save > 5000 || export_pending)) {
            uint32_t f = kspin_lock_irqsave(&lock);
            bool export = export_pending;
            export_pending = false;
            kspin_unlock_irqrestore(&lock, f);
            save_events(export);
            last_save = now;
        }
        task_sleep(pit_get_frequency_hz() / 4U ? pit_get_frequency_hz() / 4U : 1U);
    }
}
void admin_storage_ready(void) {
    vfs_mkdir("/SYSTEM/USER");
    vfs_mkdir("/SYSTEM/USER/CONFIG");
    vfs_mkdir("/SYSTEM/LOGS");
    load_config();
    storage = true;
    if (task_create_kernel("system-admin", worker, NULL) < 0)
        admin_emit(5, 2, "ADMIN_WORKER_FAILED", "No se pudo iniciar el administrador", 0);
}
void admin_desktop_ready(void) {
    desktop_ready = true;
    desktop_tick = ms();
}

bool bk_proc_end_process(uint32_t pid) {
    uint32_t f = kspin_lock_irqsave(&lock);
    bool protected = false;
    for (uint32_t i = 0; i < entry_count; i++)
        if (entries[i].critical && entries[i].pid == pid)
            protected = true;
    kspin_unlock_irqrestore(&lock, f);
    return !protected && task_request_exit_process(pid, 0);
}

uint32_t bk_proc_priority(uint32_t pid, uint32_t level) {
    return task_process_priority(pid, level);
}

static bool read_network(const char *path, bk_network_profile_t *out) {
    void *raw = NULL;
    uint32_t size = 0;
    if (!vfs_read_all(path, &raw, &size) || !raw)
        return false;
    if (size > 1024) {
        kfree(raw);
        return false;
    }
    kmemset(out, 0, sizeof(*out));
    uint32_t pos = 0, seen = 0;
    while (pos < size) {
        char line[96];
        uint32_t n = 0;
        while (pos < size && ((char *)raw)[pos] != '\n') {
            char c = ((char *)raw)[pos++];
            if (c != '\r' && n + 1 < sizeof(line))
                line[n++] = c;
        }
        pos++;
        line[n] = 0;
        char *eq = line;
        while (*eq && *eq != '=')
            eq++;
        if (!*eq)
            continue;
        *eq++ = 0;
        if (!kstrcmp(line, "dhcp")) {
            out->dhcp = eq[0] == '1';
            seen |= 1;
        } else if (!kstrcmp(line, "ip")) {
            text(out->ip, 32, eq);
            seen |= 2;
        } else if (!kstrcmp(line, "mask")) {
            text(out->mask, 32, eq);
            seen |= 4;
        } else if (!kstrcmp(line, "gateway")) {
            text(out->gateway, 32, eq);
            seen |= 8;
        } else if (!kstrcmp(line, "dns")) {
            text(out->dns, 32, eq);
            seen |= 16;
        }
    }
    kfree(raw);
    uint8_t bytes[4];
    return seen == 31 && ipv4(out->ip, bytes) && ipv4(out->mask, bytes) &&
           ipv4(out->gateway, bytes) && ipv4(out->dns, bytes);
}
bool bk_network_profile_get(uint32_t index, bk_network_profile_t *out) {
    if (index > 2)
        return false;
    char path[96];
    snprintf(path, sizeof(path), "/SYSTEM/USER/CONFIG/NETWORK%u.INI", index + 1);
    bk_network_profile_t profile;
    return read_network(path, &profile) && copy_to_user(out, &profile, sizeof(profile));
}
bool admin_network_restore(void) {
    bk_network_profile_t p;
    if (!read_network("/SYSTEM/USER/CONFIG/NETWORK.INI", &p))
        return false;
    if (p.dhcp)
        return false;
    uint8_t a[4], m[4], g[4], d[4];
    if (!ipv4(p.ip, a) || !ipv4(p.mask, m) || !ipv4(p.gateway, g) || !ipv4(p.dns, d))
        return false;
    bool ok = network_configure(a, m, g, d);
    admin_emit(4, ok ? 0 : 2, "PROFILE_RESTORE",
               ok ? "Perfil IP fijo restaurado" : "Fallo al restaurar perfil IP fijo", 0);
    return ok;
}
