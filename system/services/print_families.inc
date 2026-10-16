/* SPDX-License-Identifier: GPL-2.0-or-later
 * Capability-directed printer families. Never infer PCL from manufacturer;
 * PCL XL/PCL6-only and host-raster devices require their own encoders.
 */
static bool printer_id_value(const char *id, const char *key, char *out, uint32_t capacity) {
    if (!id || !key || !out || !capacity) return false;
    uint32_t key_length = text_length(key);
    while (*id) {
        while (*id == ' ' || *id == ';') id++;
        const char *start = id;
        while (*id && *id != ':' && *id != ';') id++;
        uint32_t n = (uint32_t)(id - start);
        bool match = n == key_length;
        for (uint32_t i = 0; match && i < n; i++)
            match = upper_ascii(start[i]) == upper_ascii(key[i]);
        if (*id == ':' && match) {
            id++; n = 0;
            while (*id && *id != ';') {
                if (n + 1U >= capacity) return false;
                out[n++] = *id++;
            }
            out[n] = '\0'; return n != 0U;
        }
        while (*id && *id != ';') id++;
    }
    return false;
}
static bool printer_command(const char *commands, const char *wanted) {
    uint32_t length = text_length(wanted);
    while (*commands) {
        while (*commands == ',' || *commands == ' ') commands++;
        const char *start = commands;
        while (*commands && *commands != ',') commands++;
        const char *end = commands;
        while (end > start && end[-1] == ' ') end--;
        bool match = (uint32_t)(end - start) == length;
        for (uint32_t i = 0; match && i < length; i++)
            match = upper_ascii(start[i]) == upper_ascii(wanted[i]);
        if (match) return true;
    }
    return false;
}
static bool printer_family_language(const char *id, print_language_t *language) {
    char commands[128], model[96];
    if (!language || (!printer_id_value(id, "CMD", commands, sizeof(commands)) &&
        !printer_id_value(id, "COMMAND SET", commands, sizeof(commands)))) return false;
    if (printer_command(commands, "POSTSCRIPT") || printer_command(commands, "POSTSCRIPT2") ||
        printer_command(commands, "POSTSCRIPT3") || printer_command(commands, "PS")) *language = LANG_POSTSCRIPT;
    else if (printer_command(commands, "PCL") || printer_command(commands, "PCL5") ||
        printer_command(commands, "PCL5E") || printer_command(commands, "PCL5C")) *language = LANG_PCL5;
    else if (printer_command(commands, "ESC/P2") || printer_command(commands, "ESCP2")) *language = LANG_ESCP2;
    else if (printer_command(commands, "ESC/P") || printer_command(commands, "ESCP")) *language = LANG_ESCP;
    else if (printer_command(commands, "ZJS") &&
        (printer_id_value(id, "MDL", model, sizeof(model)) ||
         printer_id_value(id, "MODEL", model, sizeof(model))) &&
        (text_equal_ci(model, "HP LaserJet 1020") || text_equal_ci(model, "HP LaserJet 1018") ||
         text_equal_ci(model, "LaserJet 1020") || text_equal_ci(model, "LaserJet 1018"))) *language = LANG_ZJS;
    else return false;
    return true;
}
static bool profile_auto_detect(printer_profile_t *profile) {
    uint32_t index;
    bk_print_port_info_t info;
    char id[192];
    if (profile->transport != TRANSPORT_LPT || !find_print_port(profile, &index, &info) ||
        !bk_print_device_id(index, id, sizeof(id)) ||
        !printer_family_language(id, &profile->language)) {
        bk_sys_log("[PRINT] AUTO: printer has no supported advertised command language\n");
        return false;
    }
    if (profile->language == LANG_ZJS) profile->dpi = 600U;
    return true;
}
