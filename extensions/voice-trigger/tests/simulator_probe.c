// SPDX-License-Identifier: Apache-2.0
/* Real Guest integration fixture. Injects detector events, not touch/key input.
 * The product simulator itself remains unaware of this optional extension. */
#undef NDEBUG
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <pxa_ext/voice_trigger.h>
#include <pxa/activation.h>
static pxa_status_t prepare_probe(void *, size_t, const pxa_package_manifest_t *,
    const pxa_package_activation_profile_t *, const pxa_package_host_profile_t *,
    pxa_bytes_t, pxa_activation_plan_t **);
static pxa_status_t register_probe(void *, pxa_permission_service_t *);
static void deinit_probe(pxa_runtime_t *);
#define pxa_permission_service_register(p) register_probe(&host, (p))
#define pxa_runtime_deinit deinit_probe
#define pxa_activation_plan_prepare prepare_probe
#define PXSYS_PRODUCT_RUNNER_LIBRARY 1
#include PXA_PRODUCT_RUNNER
#undef pxa_permission_service_register
#undef pxa_runtime_deinit
#undef pxa_activation_plan_prepare
#include <assert.h>

static struct {
    product_host_t *host;
    pxa_runtime_t *runtime;
    pxa_vt_service_t *service;
    void *workspace;
    pxa_vt_match_t match;
    unsigned starts, stops, command;
    pxa_status_t failure;
    int enabled, pending;
} fixture;
static pxa_status_t prepare_probe(void *workspace, size_t size,
    const pxa_package_manifest_t *manifest, const pxa_package_activation_profile_t *caps,
    const pxa_package_host_profile_t *host, pxa_bytes_t root, pxa_activation_plan_t **output) {
    pxa_status_t status = pxa_activation_plan_prepare(workspace, size, manifest, caps, host, root, output);
    if (status != PXA_STATUS_OK) {
        for (unsigned i = 0; i < manifest->components[0].service_count; ++i) {
            const pxa_package_service_requirement_t *r = &manifest->components[0].services[i];
            fprintf(stderr, "VT_PROBE_REQUIRES service=%u min=%u.%u max=%u.%u features=%llu\n",
                r->service, r->min_version.major, r->min_version.minor,
                r->max_version.major, r->max_version.minor, (unsigned long long)r->required_features);
        }
        for (unsigned i = 0; i < caps->service_count; ++i)
            fprintf(stderr, "VT_PROBE_CAP service=%u version=%u.%u features=%llu\n",
                caps->services[i].service, caps->services[i].version.major,
                caps->services[i].version.minor, (unsigned long long)caps->services[i].features);
    }
    return status;
}
static pxa_status_t profile(void *ctx, pxa_vt_profile_t *out) {
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->flags = PXA_VT_FOREGROUND_ONLY | PXA_VT_PLAYBACK_DETECTION;
    out->ready = 1; out->revision = 7; out->keyword_count = 1;
    out->keywords[0].id = 42;
    strcpy(out->keywords[0].label, "你好小智");
    return PXA_STATUS_OK;
}
static pxa_status_t start(void *ctx, uint32_t revision) {
    (void)ctx;
    assert(revision == 7 && !fixture.enabled);
    fixture.enabled = 1; fixture.pending = 0; fixture.failure = 0;
    fprintf(stderr, "VT_PROBE_SUBSCRIBED starts=%u\n", ++fixture.starts);
    return PXA_STATUS_OK;
}
static void stop(void *ctx) {
    (void)ctx;
    fixture.enabled = fixture.pending = 0;
    fprintf(stderr, "VT_PROBE_STOP stops=%u\n", ++fixture.stops);
}
static pxa_status_t read_match(void *ctx, pxa_vt_match_t *out) {
    (void)ctx;
    if (fixture.failure) return fixture.failure;
    if (!fixture.pending) return PXA_STATUS_WOULD_BLOCK;
    *out = fixture.match; fixture.pending = 0;
    return PXA_STATUS_OK;
}
static pxa_status_t register_probe(void *context, pxa_permission_service_t *permissions) {
    product_host_t *host = context;
    pxa_status_t status = pxa_permission_service_register(permissions);
    const pxa_vt_backend_t backend = {sizeof(backend), NULL, profile, start, read_match, stop};
    if (status != PXA_STATUS_OK) return status;
    fixture.host = host; fixture.runtime = host->runtime;
    fixture.workspace = malloc(pxa_vt_service_workspace_size()); assert(fixture.workspace);
    status = pxa_vt_service_init(fixture.workspace, pxa_vt_service_workspace_size(),
        host->runtime, permissions, &backend, &fixture.service);
    if (status == PXA_STATUS_OK) status = pxa_vt_service_register(fixture.service);
    return status;
}
static void deinit_probe(pxa_runtime_t *runtime) {
    pxa_runtime_deinit(runtime);
    if (runtime == fixture.runtime) {
        pxa_vt_service_deinit(fixture.service);
        free(fixture.workspace);
        fixture.workspace = NULL; fixture.service = NULL; fixture.runtime = NULL;
    }
}
static void pump_probe(void *context) {
    (void)context;
    product_host_t *host = fixture.host;
    if (!host || !fixture.service) return;
    const char *path = getenv("PXA_VT_FIXTURE_FILE");
    FILE *file = path ? fopen(path, "r") : NULL;
    if (file) {
        unsigned serial;
        char action[16];
        unsigned long long value;
        if (fscanf(file, "%u %15s %llu", &serial, action, &value) == 3 && serial != fixture.command) {
            fixture.command = serial;
            if (!strcmp(action, "trigger") && fixture.enabled) {
                fixture.match = (pxa_vt_match_t){value, now_us(NULL), 7, 42};
                fixture.pending = 1;
            } else if (!strcmp(action, "focus")) product_focus_changed(host, value != 0);
            else if (!strcmp(action, "fail")) fixture.failure = -(pxa_status_t)value;
            else if (!strcmp(action, "revoke")) {
                pxa_component_t affected[2]; size_t count;
                assert(pxa_permission_revoke(host->permissions,
                    (pxa_bytes_t){(const uint8_t *)PXA_VT_PERMISSION, sizeof(PXA_VT_PERMISSION)-1},
                    (pxa_bytes_t){(const uint8_t *)PXA_VT_DEFAULT_SCOPE, sizeof(PXA_VT_DEFAULT_SCOPE)-1},
                    affected, 2, &count) == PXA_STATUS_OK);
            }
            fprintf(stderr, "VT_PROBE_COMMAND serial=%u action=%s enabled=%d\n", serial, action, fixture.enabled);
        }
        fclose(file);
    }
    pxa_vt_service_set_foreground(fixture.service,
        host->focused ? host->active_component : PXA_COMPONENT_INVALID);
    (void)pxa_vt_service_poll(fixture.service, now_us(NULL));
}
int main(int argc, char **argv) {
    options_t options = {0};
    options.width = 320; options.height = 240; options.density_dpi = 160; options.locale = "zh-CN";
    for (int i = 1; i < argc; ++i) {
        if (i + 1 >= argc) return 2;
        const char *option = argv[i++], *value = argv[i];
        if (!strcmp(option, "--package")) options.package_path = value;
        else if (!strcmp(option, "--publisher-key")) options.publisher_key = value;
        else if (!strcmp(option, "--state-root")) options.state_root = value;
        else if (!strcmp(option, "--locale")) options.locale = value;
        else if (!strcmp(option, "--width")) options.width = (uint32_t)atoi(value);
        else if (!strcmp(option, "--height")) options.height = (uint32_t)atoi(value);
        else if (!strcmp(option, "--pxadb-control-socket")) options.pxadb_control_socket = value;
        else return 2;
    }
    if (!options.package_path || !options.publisher_key) return 2;
    return run_product_simulator(&options, NULL, NULL, pump_probe, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL);
}
