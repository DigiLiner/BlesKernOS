#ifndef BK_SDR_H
#define BK_SDR_H

#include "types.h"

#define BK_SDR_PROVIDER_ABI_VERSION 1U
#define BK_SDR_MAX_PROVIDERS 4U
#define BK_SDR_MAX_SESSIONS  8U

typedef struct {
    uint32_t struct_size;
    char name[32];
    char tuner[24];
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t min_frequency_hz;
    uint32_t max_frequency_hz;
    uint32_t frequency_hz;
    uint32_t sample_rate_hz;
    int32_t gain_tenth_db;
    bool streaming;
} sdr_device_info_t;

typedef struct sdr_provider {
    uint32_t abi_version;
    uint32_t descriptor_size;
    const char *name;
    uint32_t (*device_count)(void);
    bool (*device_info)(uint32_t index, sdr_device_info_t *info);
    int32_t (*open)(uint32_t index);
    void (*close)(int32_t handle);
    bool (*set_frequency)(int32_t handle, uint32_t frequency_hz);
    bool (*set_sample_rate)(int32_t handle, uint32_t sample_rate_hz);
    bool (*set_gain)(int32_t handle, int32_t gain_tenth_db);
    int32_t (*read)(int32_t handle, void *buffer, uint32_t capacity,
                    uint32_t timeout_ms);
} sdr_provider_t;

/*
 * Provider ABI for resident .DVR drivers.
 *
 * A provider owns its hardware synchronization and its local handles. The
 * core only multiplexes devices/sessions and never exposes driver pointers to
 * Ring 3.
 */
bool sdr_register_provider(const sdr_provider_t *provider);
bool sdr_unregister_provider(const sdr_provider_t *provider);

uint32_t sdr_device_count(void);
bool sdr_device_info(uint32_t index, sdr_device_info_t *info);
int32_t sdr_open(uint32_t index);
void sdr_close(int32_t handle);
bool sdr_set_frequency(int32_t handle, uint32_t frequency_hz);
bool sdr_set_sample_rate(int32_t handle, uint32_t sample_rate_hz);
bool sdr_set_gain(int32_t handle, int32_t gain_tenth_db);
int32_t sdr_read(int32_t handle, void *buffer, uint32_t capacity,
                 uint32_t timeout_ms);

#endif
