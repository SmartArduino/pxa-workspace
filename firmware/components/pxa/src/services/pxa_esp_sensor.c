#include "pxa/pxa_esp_sensor.h"
#include <string.h>

static pxa_sensor_config_t g_provider;

void pxa_esp_sensor_set_provider(const pxa_sensor_config_t *provider) {
    memset(&g_provider, 0, sizeof(g_provider));
    if (provider && provider->struct_size == sizeof(*provider) &&
        provider->descriptors && provider->descriptor_count &&
        provider->subscribe && provider->read && provider->unsubscribe)
        g_provider = *provider;
}

void pxa_esp_sensor_configure(pxa_sensor_config_t *config) {
    if (!config || !g_provider.descriptor_count) return;
    config->descriptors = g_provider.descriptors;
    config->descriptor_count = g_provider.descriptor_count;
    config->provider_context = g_provider.provider_context;
    config->subscribe = g_provider.subscribe;
    config->read = g_provider.read;
    config->unsubscribe = g_provider.unsubscribe;
}
