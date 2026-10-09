#!/usr/bin/env python3
"""Run IDF's component manager with a fallback for stale public storage indexes.

The registry API can publish a version before components-file.espressif.com
lists it. Keep IDF's normal resolution, download and checksum validation;
only consult the official API when its storage index has no matching version.
"""

import runpy
import sys
from urllib.parse import urljoin


def install_registry_fallback():
    from idf_component_tools.constants import (
        IDF_COMPONENT_REGISTRY_URL,
        IDF_COMPONENT_STORAGE_URL,
    )
    from idf_component_tools.messages import hint
    from idf_component_tools.registry.api_client import APIClient
    from idf_component_tools.registry.base_client import filter_versions
    from idf_component_tools.registry.client_errors import (
        ComponentNotFound,
        VersionNotFound,
    )
    from idf_component_tools.registry.request_processor import normalize_storage_url
    from idf_component_tools.registry.storage_client import StorageClient
    from idf_component_tools.utils import ComponentWithVersions

    original_versions = StorageClient.versions
    original_component = StorageClient.component
    public_storage = normalize_storage_url(IDF_COMPONENT_STORAGE_URL)

    def is_public_storage(client):
        return normalize_storage_url(client.storage_url) == public_storage

    def versions(client, component_name, spec="*"):
        try:
            result = original_versions(client, component_name, spec)
        except ComponentNotFound:
            if not is_public_storage(client):
                raise
            result = ComponentWithVersions(component_name, [])
        if result.versions or not is_public_storage(client):
            return result
        try:
            fallback = APIClient(IDF_COMPONENT_REGISTRY_URL).versions(component_name, spec)
        except ComponentNotFound:
            return result
        if fallback.versions:
            hint(f"Storage index missing {component_name} {spec}; using official registry API")
        return fallback

    def component(client, component_name, version=None):
        try:
            return original_component(client, component_name, version)
        except (ComponentNotFound, VersionNotFound):
            if not is_public_storage(client):
                raise
            response = APIClient(IDF_COMPONENT_REGISTRY_URL).get_component_response(
                component_name=component_name
            )
            matches = filter_versions(response.versions, version or "*", component_name)
            if not matches:
                raise
            metadata = matches[0].model_dump()
            metadata["name"] = component_name
            # API metadata normally contains absolute URLs. urljoin also handles
            # relative URLs while preserving the storage download/checksum path.
            metadata["download_url"] = urljoin(IDF_COMPONENT_STORAGE_URL, metadata["url"])
            metadata["checksums_url"] = urljoin(
                IDF_COMPONENT_STORAGE_URL, metadata["checksums"]
            )
            return metadata

    StorageClient.versions = versions
    StorageClient.component = component


if __name__ == "__main__":
    # ESP-IDF's IDF_COMPONENT_WRAPPER supplies the module followed by its args.
    module = sys.argv.pop(1)
    install_registry_fallback()
    runpy.run_module(module, run_name="__main__", alter_sys=True)
