#include "../../include/sdr.h"
#include "../../include/memory.h"

typedef struct {
    bool used;
    const sdr_provider_t *source;
    sdr_provider_t ops;
} sdr_provider_slot_t;

typedef struct {
    bool used;
    uint8_t provider_index;
    int32_t local_handle;
} sdr_session_t;

static sdr_provider_slot_t g_sdr_providers[BK_SDR_MAX_PROVIDERS];
static sdr_session_t g_sdr_sessions[BK_SDR_MAX_SESSIONS];

static bool sdr_provider_valid(const sdr_provider_t *provider) {
    return provider &&
           provider->abi_version == BK_SDR_PROVIDER_ABI_VERSION &&
           provider->descriptor_size >= sizeof(sdr_provider_t) &&
           provider->name && provider->name[0] &&
           provider->device_count && provider->device_info &&
           provider->open && provider->close &&
           provider->set_frequency && provider->set_sample_rate &&
           provider->set_gain && provider->read;
}

static bool sdr_resolve_device(uint32_t global_index,
                               uint8_t *provider_index,
                               uint32_t *local_index) {
    uint32_t base = 0U;

    for (uint32_t i = 0U; i < BK_SDR_MAX_PROVIDERS; i++) {
        uint32_t count;
        if (!g_sdr_providers[i].used) continue;
        count = g_sdr_providers[i].ops.device_count();
        if (global_index < base + count) {
            if (provider_index) *provider_index = (uint8_t)i;
            if (local_index) *local_index = global_index - base;
            return true;
        }
        base += count;
    }
    return false;
}

static sdr_session_t *sdr_session(int32_t handle) {
    uint32_t slot;
    if (handle <= 0) return NULL;
    slot = (uint32_t)(handle - 1);
    if (slot >= BK_SDR_MAX_SESSIONS || !g_sdr_sessions[slot].used)
        return NULL;
    return &g_sdr_sessions[slot];
}

bool sdr_register_provider(const sdr_provider_t *provider) {
    if (!sdr_provider_valid(provider)) return false;

    for (uint32_t i = 0U; i < BK_SDR_MAX_PROVIDERS; i++) {
        if (g_sdr_providers[i].used &&
            g_sdr_providers[i].source == provider)
            return true;
    }

    for (uint32_t i = 0U; i < BK_SDR_MAX_PROVIDERS; i++) {
        if (!g_sdr_providers[i].used) {
            kmemset(&g_sdr_providers[i], 0, sizeof(g_sdr_providers[i]));
            g_sdr_providers[i].source = provider;
            g_sdr_providers[i].ops = *provider;
            g_sdr_providers[i].used = true;
            return true;
        }
    }
    return false;
}

bool sdr_unregister_provider(const sdr_provider_t *provider) {
    if (!provider) return false;

    for (uint32_t i = 0U; i < BK_SDR_MAX_PROVIDERS; i++) {
        if (!g_sdr_providers[i].used ||
            g_sdr_providers[i].source != provider)
            continue;

        for (uint32_t s = 0U; s < BK_SDR_MAX_SESSIONS; s++) {
            if (g_sdr_sessions[s].used &&
                g_sdr_sessions[s].provider_index == i) {
                g_sdr_providers[i].ops.close(
                    g_sdr_sessions[s].local_handle);
                kmemset(&g_sdr_sessions[s], 0, sizeof(g_sdr_sessions[s]));
            }
        }
        kmemset(&g_sdr_providers[i], 0, sizeof(g_sdr_providers[i]));
        return true;
    }
    return false;
}

uint32_t sdr_device_count(void) {
    uint32_t total = 0U;
    for (uint32_t i = 0U; i < BK_SDR_MAX_PROVIDERS; i++)
        if (g_sdr_providers[i].used)
            total += g_sdr_providers[i].ops.device_count();
    return total;
}

bool sdr_device_info(uint32_t index, sdr_device_info_t *info) {
    uint8_t provider_index;
    uint32_t local_index;

    if (!info || info->struct_size < sizeof(*info) ||
        !sdr_resolve_device(index, &provider_index, &local_index))
        return false;

    return g_sdr_providers[provider_index].ops.device_info(local_index, info);
}

int32_t sdr_open(uint32_t index) {
    uint8_t provider_index;
    uint32_t local_index;
    int32_t local_handle;

    if (!sdr_resolve_device(index, &provider_index, &local_index))
        return -1;

    local_handle = g_sdr_providers[provider_index].ops.open(local_index);
    if (local_handle < 0) return -1;

    for (uint32_t s = 0U; s < BK_SDR_MAX_SESSIONS; s++) {
        if (!g_sdr_sessions[s].used) {
            g_sdr_sessions[s].used = true;
            g_sdr_sessions[s].provider_index = provider_index;
            g_sdr_sessions[s].local_handle = local_handle;
            return (int32_t)(s + 1U);
        }
    }

    g_sdr_providers[provider_index].ops.close(local_handle);
    return -1;
}

void sdr_close(int32_t handle) {
    sdr_session_t *session = sdr_session(handle);
    uint8_t provider_index;

    if (!session) return;
    provider_index = session->provider_index;
    if (provider_index < BK_SDR_MAX_PROVIDERS &&
        g_sdr_providers[provider_index].used)
        g_sdr_providers[provider_index].ops.close(session->local_handle);
    kmemset(session, 0, sizeof(*session));
}

bool sdr_set_frequency(int32_t handle, uint32_t frequency_hz) {
    sdr_session_t *session = sdr_session(handle);
    if (!session ||
        session->provider_index >= BK_SDR_MAX_PROVIDERS ||
        !g_sdr_providers[session->provider_index].used)
        return false;
    return g_sdr_providers[session->provider_index].ops.set_frequency(
        session->local_handle, frequency_hz);
}

bool sdr_set_sample_rate(int32_t handle, uint32_t sample_rate_hz) {
    sdr_session_t *session = sdr_session(handle);
    if (!session ||
        session->provider_index >= BK_SDR_MAX_PROVIDERS ||
        !g_sdr_providers[session->provider_index].used)
        return false;
    return g_sdr_providers[session->provider_index].ops.set_sample_rate(
        session->local_handle, sample_rate_hz);
}

bool sdr_set_gain(int32_t handle, int32_t gain_tenth_db) {
    sdr_session_t *session = sdr_session(handle);
    if (!session ||
        session->provider_index >= BK_SDR_MAX_PROVIDERS ||
        !g_sdr_providers[session->provider_index].used)
        return false;
    return g_sdr_providers[session->provider_index].ops.set_gain(
        session->local_handle, gain_tenth_db);
}

int32_t sdr_read(int32_t handle, void *buffer, uint32_t capacity,
                 uint32_t timeout_ms) {
    sdr_session_t *session = sdr_session(handle);
    if (!session || !buffer || !capacity ||
        session->provider_index >= BK_SDR_MAX_PROVIDERS ||
        !g_sdr_providers[session->provider_index].used)
        return -1;
    return g_sdr_providers[session->provider_index].ops.read(
        session->local_handle, buffer, capacity, timeout_ms);
}
