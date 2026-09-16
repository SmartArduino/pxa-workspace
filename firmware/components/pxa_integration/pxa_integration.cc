#include "pxa_integration.h"

#include <cstdlib>
#include <cstring>

#include <esp_err.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <pxa/pxa_host.h>
#include <pxsys/esp_pxa_bridge.h>
#include <pxsys/lvgl_renderer.h>
#include <pxsys/reference_layout.h>
#include <pxsys/reference_lvgl.h>
#include <pxsys/standard_system.h>

#include "pxa_board_api.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "pxa_integration";

pxsys_standard_system_t* g_system;
pxsys_lvgl_renderer_t* g_renderer;
pxsys_esp_pxa_bridge_t* g_bridge;
pxsys_reference_lvgl_t* g_reference_ui;
lv_font_t* g_text_font;
lv_font_t* g_title_font;

void* Allocate(void*, size_t size) { return std::malloc(size); }
void Release(void*, void* memory) { std::free(memory); }

bool MountPxaStorage() {
    const esp_vfs_littlefs_conf_t config = {
        .base_path = CONFIG_PXA_MOUNT_POINT,
        .partition_label = CONFIG_PXA_STORAGE_PARTITION_LABEL,
        .partition = nullptr,
        .format_if_mount_failed = CONFIG_PXA_STORAGE_FORMAT_IF_MOUNT_FAILED,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    const esp_err_t result = esp_vfs_littlefs_register(&config);
    if (result == ESP_OK || result == ESP_ERR_INVALID_STATE)
        return true;
    ESP_LOGE(kTag, "Cannot mount PXA storage '%s' at '%s': %s",
             CONFIG_PXA_STORAGE_PARTITION_LABEL, CONFIG_PXA_MOUNT_POINT,
             esp_err_to_name(result));
    return false;
}

bool ResolveAppMetadata(void* context, const pxsys_app_descriptor_t* app,
                        const pxsys_locale_snapshot_t* locale,
                        pxsys_app_metadata_t* metadata) {
    return pxsys_esp_pxa_bridge_resolve_app_metadata(
        static_cast<pxsys_esp_pxa_bridge_t*>(context), app, locale, metadata);
}

bool ResolveAppIcon(void* context, const pxsys_app_descriptor_t* app,
                    pxsys_reference_lvgl_app_icon_t* output) {
    pxa_host_icon_t icon = {};
    if (output == nullptr || !pxsys_esp_pxa_bridge_resolve_app_icon(
                                 static_cast<pxsys_esp_pxa_bridge_t*>(context),
                                 app, &icon)) {
        return false;
    }
    output->source = icon.image_dsc;
    output->release = icon.release;
    output->release_context = icon.release_context;
    return output->source != nullptr;
}

void RefreshLauncherApps(void*) {
    if (g_reference_ui != nullptr)
        pxsys_reference_lvgl_refresh_apps(g_reference_ui);
}

const lv_font_t* LoadFont(const char* path, uint16_t size, lv_font_t** owned,
                          const lv_font_t* fallback) {
#if CONFIG_LV_USE_FREETYPE
    if (path != nullptr && path[0] != '\0') {
        *owned = lv_freetype_font_create(path,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size,
            LV_FREETYPE_FONT_STYLE_NORMAL);
    }
#else
    (void)path;
    (void)size;
#endif
    return *owned != nullptr ? *owned : fallback;
}

bool CreateSystem(const pxa_board_port_t* board,
                  const pxa_product_profile_t* profile) {
    pxsys_allocator_t allocator = {};
    allocator.struct_size = sizeof(allocator);
    allocator.allocate = Allocate;
    allocator.release = Release;

    lv_display_t* display = board->display(board->context);
    if (display == nullptr) {
        ESP_LOGE(kTag, "Board did not provide an LVGL display");
        return false;
    }

    pxsys_lvgl_renderer_config_t renderer_config;
    pxsys_lvgl_renderer_config_init(&renderer_config);
    renderer_config.max_surfaces = 24;
    renderer_config.parent = lv_screen_active();
    renderer_config.allocator = allocator;
    pxsys_status_t status = pxsys_lvgl_renderer_create(&renderer_config, &g_renderer);
    if (status != PXSYS_STATUS_OK) {
        ESP_LOGE(kTag, "LVGL renderer failed: %s", pxsys_status_name(status));
        return false;
    }

    pxsys_renderer_provider_t renderer_provider;
    status = pxsys_lvgl_renderer_provider(g_renderer, &renderer_provider);
    if (status != PXSYS_STATUS_OK)
        return false;

    pxsys_standard_system_config_t system_config;
    pxsys_standard_system_config_init(&system_config);
    system_config.max_apps = profile->max_apps;
    system_config.max_instances = profile->max_instances;
    system_config.max_tasks = profile->max_tasks;
    system_config.allocator = allocator;
    system_config.initial_renderer = &renderer_provider;
    pxsys_theme_snapshot_init(&system_config.initial_theme, profile->display_scheme);
    system_config.initial_theme.configured_mode =
        profile->display_scheme == PXSYS_COLOR_SCHEME_DARK
            ? PXSYS_THEME_MODE_DARK : PXSYS_THEME_MODE_LIGHT;
    if (pxsys_locale_snapshot_init(&system_config.initial_locale,
            pxsys_string_from_cstr(profile->locale)) != PXSYS_STATUS_OK) {
        return false;
    }
    board->display_profile(board->context, &system_config.initial_display);
    system_config.network_control_context = board->context;
    system_config.set_network_enabled = board->set_network_enabled;
    system_config.control_context = board->context;
    system_config.set_level = board->set_level;
    status = pxsys_standard_system_create(&system_config, &g_system);
    if (status != PXSYS_STATUS_OK) {
        ESP_LOGE(kTag, "Standard system failed: %s", pxsys_status_name(status));
        return false;
    }

    pxsys_esp_pxa_bridge_config_t bridge_config;
    pxsys_esp_pxa_bridge_config_init(&bridge_config);
    bridge_config.system = g_system;
    bridge_config.allocator = allocator;
    bridge_config.catalog_synced = RefreshLauncherApps;
    status = pxsys_esp_pxa_bridge_create(&bridge_config, &g_bridge);
    if (status != PXSYS_STATUS_OK) {
        ESP_LOGE(kTag, "PXA bridge failed: %s", pxsys_status_name(status));
        return false;
    }

    const lv_font_t* text_font = LoadFont(profile->font_path, 14, &g_text_font,
                                           &lv_font_montserrat_14);
    const lv_font_t* title_font = LoadFont(profile->font_path, 20, &g_title_font,
                                            &lv_font_montserrat_20);
    pxsys_reference_lvgl_config_t ui_config;
    pxsys_reference_lvgl_config_init(&ui_config);
    ui_config.system = g_system;
    ui_config.parent = lv_display_get_layer_top(display);
    ui_config.text_font = text_font;
    ui_config.title_font = title_font;
    for (size_t i = 0; i < PXSYS_TYPOGRAPHY_ROLE_COUNT; ++i)
        ui_config.fonts[i] = i < PXSYS_TYPOGRAPHY_TITLE ? text_font : title_font;
    ui_config.features = PXSYS_REFERENCE_UI_HOME | PXSYS_REFERENCE_UI_SETTINGS;
#if CONFIG_PXSYS_REFERENCE_UI_STATUS_BAR
    ui_config.features |= PXSYS_REFERENCE_UI_STATUS_BAR;
#endif
#if CONFIG_PXSYS_REFERENCE_UI_NAVIGATION_BAR
    ui_config.features |= PXSYS_REFERENCE_UI_NAVIGATION_BAR;
#endif
#if CONFIG_PXSYS_REFERENCE_UI_NOTIFICATION_SHADE
    ui_config.features |= PXSYS_REFERENCE_UI_NOTIFICATION_SHADE;
#endif
#if CONFIG_PXSYS_REFERENCE_UI_WALLPAPER
    ui_config.features |= PXSYS_REFERENCE_UI_WALLPAPER;
#endif
#if CONFIG_PXSYS_REFERENCE_UI_NAVIGATION_GESTURES
    ui_config.navigation_mode = PXSYS_NAVIGATION_GESTURES;
#else
    ui_config.navigation_mode = PXSYS_NAVIGATION_BUTTONS;
#endif
    ui_config.allocator = allocator;
    std::memset(ui_config.publisher_root, 0x52, sizeof(ui_config.publisher_root));
    ui_config.app_metadata_context = g_bridge;
    ui_config.resolve_app_metadata = ResolveAppMetadata;
    ui_config.app_icon_context = g_bridge;
    ui_config.resolve_app_icon = ResolveAppIcon;
    status = pxsys_reference_lvgl_create(&ui_config, &g_reference_ui);
    if (status == PXSYS_STATUS_OK)
        status = pxsys_reference_lvgl_start(g_reference_ui);
    if (status != PXSYS_STATUS_OK) {
        ESP_LOGE(kTag, "Reference UI failed: %s", pxsys_status_name(status));
        return false;
    }

    pxsys_reference_layout_t layout;
    pxa_window_insets_t safe = {};
    pxa_window_insets_t bars = {};
    safe.top = system_config.initial_display.safe_insets.top;
    safe.right = system_config.initial_display.safe_insets.right;
    safe.bottom = system_config.initial_display.safe_insets.bottom;
    safe.left = system_config.initial_display.safe_insets.left;
    if (pxsys_reference_layout_compute(&system_config.initial_display, &layout) ==
        PXSYS_STATUS_OK) {
        bars.top = layout.status_bar.y + layout.status_bar.height;
#if !CONFIG_PXSYS_REFERENCE_UI_NAVIGATION_GESTURES
        bars.bottom = system_config.initial_display.height - layout.navigation_bar.y;
#endif
    }
    (void)pxa_host_set_window_insets(&safe, &bars);
    if (board->system_ready != nullptr)
        board->system_ready(board->context, g_system, g_reference_ui);
    return true;
}
}  // namespace

extern "C" void pxa_product_profile_init(pxa_product_profile_t* profile) {
    if (profile == nullptr) return;
    std::memset(profile, 0, sizeof(*profile));
    profile->struct_size = sizeof(*profile);
    profile->locale = "en-US";
    profile->display_scheme = PXSYS_COLOR_SCHEME_DARK;
    profile->max_apps = 80;
    profile->max_instances = 24;
    profile->max_tasks = 24;
    profile->mount_storage = true;
}

extern "C" bool pxa_integration_start(const pxa_product_profile_t* profile) {
    const pxa_board_port_t* board = pxa_board_current();
    if (profile == nullptr || profile->struct_size != sizeof(*profile) ||
        board == nullptr || g_system != nullptr)
        return false;
    if (profile->mount_storage && !MountPxaStorage()) return false;
    if (!board->initialize(board->context) || !pxa_host_initialize()) return false;
    if (!lvgl_port_lock(2000)) return false;
    lv_lock();
    const bool started = CreateSystem(board, profile);
    lv_unlock();
    lvgl_port_unlock();
    if (!started) {
        pxa_integration_stop();
        return false;
    }
    if (board->show_initial_frame != nullptr)
        board->show_initial_frame(board->context);
    if (board->configure_diagnostics != nullptr &&
        !board->configure_diagnostics(board->context))
        ESP_LOGW(kTag, "Board diagnostics are unavailable");
    if (!pxa_host_start_runtime()) return false;
    (void)pxa_host_scan_packages();
    ESP_LOGI(kTag, "PXA System started through board port");
    return true;
}

extern "C" void pxa_integration_stop(void) {
    if (g_reference_ui != nullptr) {
        (void)pxsys_reference_lvgl_destroy(g_reference_ui);
        g_reference_ui = nullptr;
    }
    if (g_bridge != nullptr) {
        (void)pxsys_esp_pxa_bridge_destroy(g_bridge);
        g_bridge = nullptr;
    }
    if (g_system != nullptr) {
        (void)pxsys_standard_system_destroy(g_system);
        g_system = nullptr;
    }
    if (g_renderer != nullptr) {
        (void)pxsys_lvgl_renderer_destroy(g_renderer);
        g_renderer = nullptr;
    }
#if CONFIG_LV_USE_FREETYPE
    if (g_text_font != nullptr) lv_freetype_font_delete(g_text_font);
    if (g_title_font != nullptr) lv_freetype_font_delete(g_title_font);
#endif
    g_text_font = nullptr;
    g_title_font = nullptr;
}
