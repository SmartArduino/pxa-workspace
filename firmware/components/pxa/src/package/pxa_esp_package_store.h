#ifndef PXA_ESP_PACKAGE_STORE_H
#define PXA_ESP_PACKAGE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pxa/package.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PXA_ESP_PACKAGE_APP_ID_BYTES 65u
/* 64 lowercase publisher-root hex bytes, ':', 64-byte App ID, NUL. */
#define PXA_ESP_PACKAGE_ID_BYTES 130u
#define PXA_ESP_PACKAGE_NAME_BYTES 65u
#define PXA_ESP_PACKAGE_VERSION_BYTES 32u
#define PXA_ESP_PACKAGE_ROOT_BYTES 160u
#define PXA_ESP_PACKAGE_ICON_PATH_BYTES 257u
#define PXA_ESP_PACKAGE_PERMISSION_TEXT_BYTES 97u
#define PXA_ESP_PACKAGE_DESCRIPTION_BYTES 193u
#define PXA_ESP_PACKAGE_PREVIEW_PERMISSION_MAX 16u

typedef enum {
    PXA_ESP_PACKAGE_VIEW_RUNNABLE = 0,
    PXA_ESP_PACKAGE_VIEW_MANAGED,
} pxa_esp_package_view_t;

typedef enum {
    PXA_ESP_PACKAGE_ICON_INSTALLED = 0,
    PXA_ESP_PACKAGE_ICON_STAGED,
} pxa_esp_package_icon_variant_t;

typedef struct {
    /* Canonical text key: <publisher-root-hex>:<app-id>. */
    char id[PXA_ESP_PACKAGE_ID_BYTES];
    char app_id[PXA_ESP_PACKAGE_APP_ID_BYTES];
    char name[PXA_ESP_PACKAGE_NAME_BYTES];
    char version[PXA_ESP_PACKAGE_VERSION_BYTES];
    uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES];
    bool has_publisher_root;
    char staged_version[PXA_ESP_PACKAGE_VERSION_BYTES];
    uint64_t release_sequence;
    uint64_t staged_release_sequence;
    uint8_t permission_count;
    uint8_t granted_permission_count;
    bool built_in;
    bool installed;
    bool staged;
    bool enabled;
    bool has_private_data;
    bool has_release_sequence;
    bool has_staged_release_sequence;
    bool downgrade_warning;
    bool signer_rollback_warning;
} pxa_esp_package_record_t;

typedef bool (*pxa_esp_package_record_visitor_fn)(
    const pxa_esp_package_record_t *record, void *user_data);

typedef struct {
    char root[PXA_ESP_PACKAGE_ROOT_BYTES];
    char relative_path[PXA_ESP_PACKAGE_ICON_PATH_BYTES];
} pxa_esp_package_icon_source_t;

typedef struct {
    char name[PXA_ESP_PACKAGE_PERMISSION_TEXT_BYTES];
    char scope[PXA_ESP_PACKAGE_PERMISSION_TEXT_BYTES];
    bool required;
    bool granted;
} pxa_esp_package_permission_info_t;

typedef struct {
    char id[PXA_ESP_PACKAGE_ID_BYTES];
    char app_id[PXA_ESP_PACKAGE_APP_ID_BYTES];
    char name[PXA_ESP_PACKAGE_NAME_BYTES];
    char version[PXA_ESP_PACKAGE_VERSION_BYTES];
    char description[PXA_ESP_PACKAGE_DESCRIPTION_BYTES];
    uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES];
    bool has_publisher_root;
    pxa_esp_package_permission_info_t
        permissions[PXA_ESP_PACKAGE_PREVIEW_PERMISSION_MAX];
    uint8_t permission_count;
} pxa_esp_package_preview_t;

typedef struct {
    pxa_status_t status;
    const char *stage;
} pxa_esp_package_store_deploy_result_t;

typedef bool (*pxa_esp_package_permission_visitor_fn)(
    const pxa_esp_package_permission_info_t *permission, void *user_data);
typedef void (*pxa_esp_package_store_sync_fn)(void *user_data);

/* ESP32 signed package store: installs, lists and manages verified packages
 * on the LittleFS VFS. Canonical identity is (publisher root, App ID).
 * Replaces the legacy C++ package-store implementation on the C stack. */

bool pxa_esp_package_store_initialize(void);
/* Called after a built-in synchronization has released repository locks. */
void pxa_esp_package_store_set_sync_callback(
    pxa_esp_package_store_sync_fn callback, void *user_data);
void pxa_esp_package_store_sync_builtins(void);
bool pxa_esp_package_store_sync_builtins_async(void);

bool pxa_esp_package_store_is_enabled(const char *identity);
bool pxa_esp_package_store_set_enabled(const char *identity, bool enabled);
/* Moves a verified .pxa file into the private Inbox for later user approval. */
bool pxa_esp_package_store_stage_file(const char *source_path);
bool pxa_esp_package_store_install(const char *identity);
/* Installs a signed .pxa directly from a user-visible source file. The source
 * remains in place; it is not staged in or removed from the private Inbox. */
bool pxa_esp_package_store_install_file(const char *source_path);
bool pxa_esp_package_store_preview_file(const char *source_path,
                                        pxa_esp_package_preview_t *preview);
/* Development deployment bypasses Inbox duplicate filtering but still runs
 * the normal signed, atomic package installer. */
bool pxa_esp_package_store_deploy(const char *identity);
bool pxa_esp_package_store_deploy_detailed(
    const char *identity, pxa_esp_package_store_deploy_result_t *result);
bool pxa_esp_package_store_uninstall(const char *identity);
bool pxa_esp_package_store_clear_data(const char *identity);

/* Load the currently installed package; outputs live in the caller buffers
 * (root path, encoded manifest bytes, parsed manifest in workspace). */
bool pxa_esp_package_store_load_installed(
    const char *identity, char *root, size_t root_capacity,
    void *manifest_workspace, size_t manifest_workspace_size,
    uint8_t *encoded, size_t encoded_capacity,
    pxa_package_manifest_t **manifest);

/* Upper-bound workspace required to parse any manifest accepted by this store. */
size_t pxa_esp_package_store_manifest_workspace_size(void);
/* Current encoded manifest size, or zero when the Package is unavailable. */
size_t pxa_esp_package_store_installed_manifest_size(const char *identity);
/* Workspace required by the currently installed Package's actual manifest, or
 * zero when the manifest cannot be read or measured. */
size_t pxa_esp_package_store_installed_manifest_workspace_size(
    const char *identity);

/* The visitor runs while the metadata snapshot is locked. It must only copy
 * the record and must not call back into the Package Store or perform I/O. */
size_t pxa_esp_package_store_count(pxa_esp_package_view_t view);
size_t pxa_esp_package_store_visit(
    pxa_esp_package_view_t view, pxa_esp_package_record_visitor_fn visitor,
    void *user_data);
/* Rescan and authenticate staged Inbox sources. Snapshot visits stay read-only. */
bool pxa_esp_package_store_refresh_inbox(void);
bool pxa_esp_package_store_icon_source(
    const char *identity, pxa_esp_package_icon_variant_t variant,
    pxa_esp_package_icon_source_t *source);
size_t pxa_esp_package_store_permission_count(const char *identity);
size_t pxa_esp_package_store_visit_permissions(
    const char *identity, pxa_esp_package_permission_visitor_fn visitor,
    void *user_data);
bool pxa_esp_package_store_set_permission(const char *identity,
                                          size_t permission_index,
                                          bool granted);
bool pxa_esp_package_store_refresh_permission_summary(const char *identity);

/* Expose the installer for the host (install path only). */
typedef struct pxa_posix_installer pxa_posix_installer_t;
pxa_posix_installer_t *pxa_esp_package_store_installer(void);

#ifdef __cplusplus
}
#endif

#endif
