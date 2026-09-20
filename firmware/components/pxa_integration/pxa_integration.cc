#include "pxa_integration.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_app_desc.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <pxa/pxa_host.h>
#include <pxa/version.h>
#include <pxadb/pxadb_service.h>
#include <pxsys/esp_pxa_bridge.h>
#include <pxsys/lvgl_renderer.h>
#include <pxsys/reference_layout.h>
#include <pxsys/reference_lvgl.h>
#include <pxsys/standard_system.h>

#include "pxa_board_api.h"
#include "wifi_manager.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "pxa_integration";

pxsys_standard_system_t* g_system;
pxsys_lvgl_renderer_t* g_renderer;
pxsys_esp_pxa_bridge_t* g_bridge;
pxsys_reference_lvgl_t* g_reference_ui;
lv_font_t* g_typography_fonts[PXSYS_TYPOGRAPHY_ROLE_COUNT];
uint32_t g_display_width = 0;
uint32_t g_display_height = 0;

constexpr uint16_t kTypographyFontSizes[PXSYS_TYPOGRAPHY_ROLE_COUNT] = {
    28, 24, 20, 16, 14, 12,
};

const lv_font_t* const kTypographySymbolFallbacks[
    PXSYS_TYPOGRAPHY_ROLE_COUNT] = {
        &lv_font_montserrat_20,
        &lv_font_montserrat_20,
        &lv_font_montserrat_20,
        &lv_font_montserrat_14,
        &lv_font_montserrat_14,
        &lv_font_montserrat_14,
};

void* Allocate(void*, size_t size) { return std::malloc(size); }
void Release(void*, void* memory) { std::free(memory); }

bool ReadMemoryInfo(void*, uint64_t* available_bytes, uint64_t* total_bytes) {
    if (available_bytes == nullptr || total_bytes == nullptr) return false;
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    *available_bytes =
        heap_caps_get_free_size(internal_caps) +
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    *total_bytes =
        heap_caps_get_total_size(internal_caps) +
        heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    return *total_bytes != 0;
}

bool DeveloperGet(void* context, pxsys_reference_performance_option_t option) {
    const auto* board = static_cast<const pxa_board_port_t*>(context);
    if (option == PXSYS_REFERENCE_PXADB)
        return pxa_board_performance_get(option);
    return board != nullptr && board->performance_get != nullptr &&
           board->performance_get(board->context, option);
}

bool DeveloperSet(void* context, pxsys_reference_performance_option_t option,
                  bool enabled) {
    const auto* board = static_cast<const pxa_board_port_t*>(context);
    if (option == PXSYS_REFERENCE_PXADB) {
        return pxadb::SetEnabled(enabled) &&
               pxa_board_performance_set(option, enabled);
    }
    return board != nullptr && board->performance_set != nullptr &&
           board->performance_set(board->context, option, enabled);
}

size_t ScanWifi(void*, pxsys_reference_wifi_network_t* networks,
                size_t capacity) {
    if (networks == nullptr || capacity == 0) return 0;
    std::vector<WifiNetwork> found;
    if (!WifiManager::GetInstance().ScanNetworks(&found)) return 0;
    const size_t count = found.size() < capacity ? found.size() : capacity;
    for (size_t index = 0; index < count; ++index) {
        std::snprintf(networks[index].ssid, sizeof(networks[index].ssid), "%s",
                      found[index].ssid.c_str());
        networks[index].rssi = found[index].rssi;
        networks[index].secured = found[index].authmode != WIFI_AUTH_OPEN;
    }
    return count;
}

bool ConnectWifi(void*, const char* ssid, const char* password) {
    return ssid != nullptr && WifiManager::GetInstance().Connect(
        ssid, password != nullptr ? password : "");
}

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
    if (*owned == nullptr) {
        ESP_LOGW(kTag, "Font '%s' unavailable; using the built-in face",
                 path != nullptr ? path : "");
        return fallback;
    }
    ESP_LOGI(kTag, "Loaded font '%s' at %u px",
             path != nullptr ? path : "", static_cast<unsigned>(size));
    /* The CJK face covers text but not the Font Awesome private-use glyphs the
     * reference UI uses for icons; keep the built-in font as fallback. */
    (*owned)->fallback = fallback;
    return *owned;
}

#if CONFIG_PXSYS_REFERENCE_UI_BUILTIN_SETTINGS
void CopyText(char* destination, size_t capacity, const char* source) {
    size_t index = 0;
    if (capacity == 0) return;
    if (source != nullptr) {
        for (; index + 1 < capacity && source[index] != '\0'; ++index)
            destination[index] = source[index];
    }
    destination[index] = '\0';
}

void FillDeviceInfo(
    void*, char values[PXSYS_REFERENCE_DEVICE_FIELD_COUNT]
                      [PXSYS_REFERENCE_DEVICE_VALUE_MAX]) {
    const esp_app_desc_t* app = esp_app_get_description();
    if (app != nullptr) {
        std::snprintf(values[PXSYS_REFERENCE_DEVICE_FIRMWARE_NAME],
                      PXSYS_REFERENCE_DEVICE_VALUE_MAX, "%s",
                      app->project_name);
        std::snprintf(values[PXSYS_REFERENCE_DEVICE_FIRMWARE_VERSION],
                      PXSYS_REFERENCE_DEVICE_VALUE_MAX, "%s", app->version);
    }
    std::snprintf(values[PXSYS_REFERENCE_DEVICE_SYSTEM_VERSION],
                  PXSYS_REFERENCE_DEVICE_VALUE_MAX, "PXA %s",
                  PXA_VERSION_STRING);
    std::snprintf(values[PXSYS_REFERENCE_DEVICE_DISPLAY],
                  PXSYS_REFERENCE_DEVICE_VALUE_MAX, "%lu x %lu",
                  static_cast<unsigned long>(g_display_width),
                  static_cast<unsigned long>(g_display_height));
    std::snprintf(values[PXSYS_REFERENCE_DEVICE_MEMORY],
                  PXSYS_REFERENCE_DEVICE_VALUE_MAX, "SRAM %.0fK  PSRAM %.1fM",
                  (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0,
                  (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) /
                      (1024.0 * 1024.0));
    {
        size_t total = 0;
        size_t used = 0;
        if (esp_littlefs_info(CONFIG_PXA_STORAGE_PARTITION_LABEL, &total,
                              &used) == ESP_OK) {
            std::snprintf(values[PXSYS_REFERENCE_DEVICE_STORAGE],
                          PXSYS_REFERENCE_DEVICE_VALUE_MAX,
                          "%.1f / %.1f MB",
                          (double)used / (1024.0 * 1024.0),
                          (double)total / (1024.0 * 1024.0));
        }
    }
}

size_t ListManagedApps(void*,
                       pxsys_reference_managed_app_t* apps,
                       size_t capacity) {
    const size_t total = pxa_host_package_count();
    if (apps == nullptr) return total;
    if (capacity == 0) return 0;
    if (capacity > total) capacity = total;
    pxa_host_package_info_t* packages =
        static_cast<pxa_host_package_info_t*>(std::calloc(
            capacity, sizeof(pxa_host_package_info_t)));
    if (packages == nullptr) return 0;
    const size_t listed = pxa_host_list_packages(packages, capacity);
    for (size_t index = 0; index < listed; ++index) {
        const pxa_host_package_info_t* info = &packages[index];
        pxsys_reference_managed_app_t* app = &apps[index];
        std::memset(app, 0, sizeof(*app));
        std::snprintf(app->identity, sizeof(app->identity), "%s", info->id);
        std::snprintf(app->version, sizeof(app->version), "%s", info->version);
        app->built_in = info->built_in ? 1 : 0;
        app->installed = info->installed ? 1 : 0;
        app->enabled = info->enabled ? 1 : 0;
        app->has_private_data = info->has_private_data ? 1 : 0;
        app->active = info->active ? 1 : 0;
        bool named = false;
        if (info->has_publisher_root) {
            pxa_host_package_metadata_t metadata = {};
            if (pxa_host_resolve_package_metadata(
                    info->publisher_root, info->app_id, "zh-CN", &metadata) &&
                metadata.name[0] != '\0') {
                CopyText(app->name, sizeof(app->name), metadata.name);
                named = true;
            }
        }
        if (!named) {
            CopyText(app->name, sizeof(app->name),
                     info->name[0] != '\0' ? info->name : info->app_id);
        }
    }
    std::free(packages);
    return listed;
}

bool ManageApp(void*, const char* identity,
               pxsys_reference_app_action_t action) {
    if (identity == nullptr || identity[0] == '\0') return false;
    pxa_host_app_action_t mapped;
    switch (action) {
        case PXSYS_REFERENCE_APP_ACTION_ENABLE:
            mapped = PXA_HOST_APP_ACTION_ENABLE;
            break;
        case PXSYS_REFERENCE_APP_ACTION_DISABLE:
            mapped = PXA_HOST_APP_ACTION_DISABLE;
            break;
        case PXSYS_REFERENCE_APP_ACTION_CLEAR_DATA:
            mapped = PXA_HOST_APP_ACTION_CLEAR_DATA;
            break;
        case PXSYS_REFERENCE_APP_ACTION_UNINSTALL:
            mapped = PXA_HOST_APP_ACTION_UNINSTALL;
            break;
        default:
            return false;
    }
    return pxa_host_manage_app(mapped, identity);
}

bool BuildAbsolutePath(const char* logical, char* output, size_t capacity) {
    if (logical == nullptr || std::strstr(logical, "..") != nullptr)
        return false;
    const int written = std::snprintf(output, capacity, "%s%s",
                                      CONFIG_PXA_MOUNT_POINT, logical);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

size_t ListFiles(void*, const char* path, pxsys_reference_file_entry_t* entries,
                 size_t capacity) {
    char absolute[PXSYS_REFERENCE_FILE_PATH_MAX + 64];
    if (!BuildAbsolutePath(path, absolute, sizeof(absolute))) return 0;
    DIR* directory = opendir(absolute);
    if (directory == nullptr) return 0;
    size_t count = 0;
    struct dirent* item;
    while ((item = readdir(directory)) != nullptr) {
        if (std::strcmp(item->d_name, ".") == 0 ||
            std::strcmp(item->d_name, "..") == 0)
            continue;
        if (entries != nullptr) {
            if (count >= capacity) break;
            pxsys_reference_file_entry_t* entry = &entries[count];
            std::memset(entry, 0, sizeof(*entry));
            CopyText(entry->name, sizeof(entry->name), item->d_name);
            {
                size_t used = 0;
                CopyText(entry->path, sizeof(entry->path),
                         path != nullptr ? path : "");
                used = std::strlen(entry->path);
                if (used + 1 < sizeof(entry->path)) {
                    entry->path[used++] = '/';
                    CopyText(entry->path + used, sizeof(entry->path) - used,
                             item->d_name);
                }
            }
            char child[sizeof(absolute)];
            {
                size_t used = 0;
                CopyText(child, sizeof(child), absolute);
                used = std::strlen(child);
                if (used + 1 < sizeof(child)) {
                    child[used++] = '/';
                    CopyText(child + used, sizeof(child) - used, item->d_name);
                }
            }
            struct stat info = {};
            if (stat(child, &info) == 0) {
                entry->is_directory = S_ISDIR(info.st_mode) ? 1 : 0;
                entry->size = static_cast<uint64_t>(info.st_size);
            }
        }
        ++count;
    }
    closedir(directory);
    return count;
}

bool RemoveTree(const char* absolute, int depth) {
    if (depth > 16) return false;
    struct stat info = {};
    if (stat(absolute, &info) != 0) return false;
    if (S_ISDIR(info.st_mode)) {
        DIR* directory = opendir(absolute);
        if (directory == nullptr) return false;
        bool ok = true;
        struct dirent* item;
        while ((item = readdir(directory)) != nullptr) {
            if (std::strcmp(item->d_name, ".") == 0 ||
                std::strcmp(item->d_name, "..") == 0)
                continue;
            char child[PXSYS_REFERENCE_FILE_PATH_MAX + 256];
            std::snprintf(child, sizeof(child), "%s/%s", absolute,
                          item->d_name);
            if (!RemoveTree(child, depth + 1)) ok = false;
        }
        closedir(directory);
        if (!ok) return false;
    }
    return remove(absolute) == 0;
}

bool ManageFile(void*, const char* path,
                pxsys_reference_file_action_t action) {
    char absolute[PXSYS_REFERENCE_FILE_PATH_MAX + 64];
    if (action != PXSYS_REFERENCE_FILE_ACTION_DELETE) return false;
    if (path == nullptr || path[0] == '\0') return false;
    if (!BuildAbsolutePath(path, absolute, sizeof(absolute))) return false;
    if (std::strcmp(absolute, CONFIG_PXA_MOUNT_POINT) == 0) return false;
    return RemoveTree(absolute, 0);
}
#endif  // CONFIG_PXSYS_REFERENCE_UI_BUILTIN_SETTINGS

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
    g_display_width = system_config.initial_display.width;
    g_display_height = system_config.initial_display.height;
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

    pxsys_reference_lvgl_config_t ui_config;
    pxsys_reference_lvgl_config_init(&ui_config);
    ui_config.system = g_system;
    ui_config.parent = lv_display_get_layer_top(display);
    for (size_t i = 0; i < PXSYS_TYPOGRAPHY_ROLE_COUNT; ++i) {
        ui_config.fonts[i] = LoadFont(profile->font_path,
                                      kTypographyFontSizes[i],
                                      &g_typography_fonts[i],
                                      kTypographySymbolFallbacks[i]);
    }
    ui_config.text_font = ui_config.fonts[PXSYS_TYPOGRAPHY_BODY];
    ui_config.title_font = ui_config.fonts[PXSYS_TYPOGRAPHY_HEADLINE];
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
#if CONFIG_PXSYS_REFERENCE_UI_BUILTIN_SETTINGS
    ui_config.features |= PXSYS_REFERENCE_UI_SOUND_SETTINGS |
                          PXSYS_REFERENCE_UI_DEVICE_INFO |
                          PXSYS_REFERENCE_UI_APP_MANAGER |
                          PXSYS_REFERENCE_UI_FILE_MANAGER;
    ui_config.device_info = FillDeviceInfo;
    ui_config.app_list = ListManagedApps;
    ui_config.app_action = ManageApp;
    ui_config.file_list = ListFiles;
    ui_config.file_action = ManageFile;
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
    ui_config.memory_info = ReadMemoryInfo;
    ui_config.performance_context = const_cast<pxa_board_port_t*>(board);
    ui_config.performance_get = DeveloperGet;
    ui_config.performance_set = DeveloperSet;
    ui_config.wifi_scan = ScanWifi;
    ui_config.wifi_connect = ConnectWifi;
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
#if CONFIG_PXSYS_REFERENCE_UI_NAVIGATION_GESTURES
        /* Gesture mode reserves an invisible Home strip at the bottom and a
         * Back strip at the left edge. Report both so fullscreen applications
         * keep their controls out of the system gesture zones. */
        bars.bottom =
            pxsys_reference_layout_gesture_strip_height(layout.size_class);
        bars.left = pxsys_reference_layout_back_gesture_width();
#else
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
    /* The runtime performs the initial built-in package scan on its own task.
     * Do not repeat it here: this startup task must return so the UI can keep
     * presenting while Wi-Fi associates and the catalog is populated. */
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
    for (size_t i = 0; i < PXSYS_TYPOGRAPHY_ROLE_COUNT; ++i) {
        if (g_typography_fonts[i] != nullptr)
            lv_freetype_font_delete(g_typography_fonts[i]);
    }
#endif
    std::memset(g_typography_fonts, 0, sizeof(g_typography_fonts));
}
