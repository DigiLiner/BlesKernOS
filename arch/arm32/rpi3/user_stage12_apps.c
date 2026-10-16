#include "user_stage12_apps.h"
#include "elf_arm.h"

#define APP_DECL(sym) \
    extern const uint8_t rpi3_stage12_##sym##_start[]; \
    extern const uint8_t rpi3_stage12_##sym##_end[]
APP_DECL(about); APP_DECL(help); APP_DECL(dir); APP_DECL(ls); APP_DECL(cat);
APP_DECL(type); APP_DECL(echo); APP_DECL(calc); APP_DECL(uptime); APP_DECL(uname);
APP_DECL(tree); APP_DECL(hexdump); APP_DECL(checksum); APP_DECL(find); APP_DECL(sleep);
APP_DECL(attrib); APP_DECL(chmod); APP_DECL(copy); APP_DECL(cp); APP_DECL(delete);
APP_DECL(rm); APP_DECL(mkdir); APP_DECL(rmdir); APP_DECL(move); APP_DECL(rename);
APP_DECL(touch); APP_DECL(more); APP_DECL(strings); APP_DECL(diff); APP_DECL(mem);
APP_DECL(benchmark); APP_DECL(compress); APP_DECL(extract); APP_DECL(backup);

#define APP_ROW(text, sym) { text, rpi3_stage12_##sym##_start, rpi3_stage12_##sym##_end }
typedef struct arm_app_blob {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} arm_app_blob_t;
static const arm_app_blob_t g_apps[] = {
    APP_ROW("about", about),
    APP_ROW("help", help),
    APP_ROW("dir", dir),
    APP_ROW("ls", ls),
    APP_ROW("cat", cat),
    APP_ROW("type", type),
    APP_ROW("echo", echo),
    APP_ROW("calc", calc),
    APP_ROW("uptime", uptime),
    APP_ROW("uname", uname),
    APP_ROW("tree", tree),
    APP_ROW("hexdump", hexdump),
    APP_ROW("checksum", checksum),
    APP_ROW("find", find),
    APP_ROW("sleep", sleep),
    APP_ROW("attrib", attrib),
    APP_ROW("chmod", chmod),
    APP_ROW("copy", copy),
    APP_ROW("cp", cp),
    APP_ROW("delete", delete),
    APP_ROW("rm", rm),
    APP_ROW("mkdir", mkdir),
    APP_ROW("rmdir", rmdir),
    APP_ROW("move", move),
    APP_ROW("rename", rename),
    APP_ROW("touch", touch),
    APP_ROW("more", more),
    APP_ROW("strings", strings),
    APP_ROW("diff", diff),
    APP_ROW("mem", mem),
    APP_ROW("benchmark", benchmark),
    APP_ROW("compress", compress),
    APP_ROW("extract", extract),
    APP_ROW("backup", backup)
};

static char lower_ascii(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c; }
static bool equal_command(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b && lower_ascii(*a) == lower_ascii(*b)) { ++a; ++b; }
    return !*a && !*b;
}

uint32_t rpi3_stage12_command_count(void) {
    return (uint32_t)(sizeof(g_apps) / sizeof(g_apps[0]));
}
const char *rpi3_stage12_command_name(uint32_t index) {
    return index < rpi3_stage12_command_count() ? g_apps[index].name : NULL;
}
int rpi3_stage12_spawn_command(const char *command, const char *launch_line) {
    uint32_t i;
    for (i = 0U; i < rpi3_stage12_command_count(); ++i) {
        uint32_t bytes;
        if (!equal_command(command, g_apps[i].name)) continue;
        bytes = (uint32_t)(g_apps[i].end - g_apps[i].start);
        return rpi3_elf_spawn_memory(g_apps[i].name, g_apps[i].start, bytes,
                                     launch_line ? launch_line : command);
    }
    return -1;
}
