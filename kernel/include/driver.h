#ifndef DRIVER_H
#define DRIVER_H

#include "types.h"

#define BK_DRIVER_ABI_VERSION 3U
#define BK_DRIVER_MAX_LOADED  48U
#define BK_DRIVER_MAX_CATALOG 64U

typedef struct {
    uint32_t abi_version;
    uint32_t descriptor_size;
    uint16_t version_major;
    uint16_t version_minor;
    const char *name;
    const char *description;
    bool (*init)(void);
    void (*shutdown)(void);
} bk_driver_module_t;

typedef const bk_driver_module_t *(*bk_driver_query_t)(void);

typedef struct {
    char name[32];
    char path[260];
    const char *description;
    uint16_t version_major;
    uint16_t version_minor;
    bool active;
    void *image;
    void (*shutdown)(void);
} bk_loaded_driver_t;

typedef struct {
    char name[32];
    char path[260];
    char description[96];
    uint16_t version_major;
    uint16_t version_minor;
    bool active;
} bk_driver_catalog_entry_t;

void driver_loader_init(void);
bool driver_load(const char *path);
uint32_t driver_load_directory(const char *path);
uint32_t driver_count(void);
const bk_loaded_driver_t *driver_at(uint32_t index);
uint32_t driver_catalog_count(void);
const bk_driver_catalog_entry_t *driver_catalog_at(uint32_t index);
const char *driver_last_error(void);

#endif
