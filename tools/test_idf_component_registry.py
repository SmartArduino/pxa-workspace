import unittest
from unittest import mock

from idf_component_tools.constants import IDF_COMPONENT_STORAGE_URL
from idf_component_tools.registry.api_client import APIClient
from idf_component_tools.registry.api_models import ComponentResponse, VersionResponse
from idf_component_tools.registry.client_errors import ComponentNotFound, VersionNotFound
from idf_component_tools.registry.storage_client import StorageClient

import idf_component_registry


NAME = "espressif/esp_lvgl_adapter"


def response(version):
    return ComponentResponse(
        name="esp_lvgl_adapter", namespace="espressif", versions=[VersionResponse(
            version=version, component_hash="6" * 64,
            url=f"https://components-file.espressif.com/components/{NAME}/{version}/component.zip",
            checksums=f"https://components-file.espressif.com/components/{NAME}/{version}/CHECKSUMS.json",
            docs={}, license=None, examples=[],
        )]
    )


class RegistryFallbackTest(unittest.TestCase):
    def setUp(self):
        original_versions = StorageClient.versions
        original_component = StorageClient.component
        self.addCleanup(setattr, StorageClient, "versions", original_versions)
        self.addCleanup(setattr, StorageClient, "component", original_component)
        idf_component_registry.install_registry_fallback()
        self.client = StorageClient(IDF_COMPONENT_STORAGE_URL)

    def test_matching_storage_version_needs_no_api(self):
        with mock.patch.object(StorageClient, "get_component_response", return_value=response("0.7.2")), \
                mock.patch.object(APIClient, "versions") as api:
            result = self.client.versions(NAME, "^0.7.2")
        self.assertEqual(str(result.versions[0].version), "0.7.2")
        api.assert_not_called()

    def test_stale_index_resolves_api_version_and_hash(self):
        with mock.patch.object(StorageClient, "get_component_response", return_value=response("0.7.0")), \
                mock.patch.object(APIClient, "get_component_response", return_value=response("0.7.2")):
            result = self.client.versions(NAME, "^0.7.2")
        self.assertEqual(str(result.versions[0].version), "0.7.2")
        self.assertEqual(result.versions[0].component_hash, "6" * 64)

    def test_uncached_download_keeps_absolute_archive_and_checksum_urls(self):
        with mock.patch.object(StorageClient, "get_component_response", return_value=response("0.7.0")), \
                mock.patch.object(APIClient, "get_component_response", return_value=response("0.7.2")):
            metadata = self.client.component(NAME, "0.7.2")
        self.assertEqual(metadata["download_url"], response("0.7.2").versions[0].url)
        self.assertEqual(metadata["checksums_url"], response("0.7.2").versions[0].checksums)
        self.assertEqual(metadata["component_hash"], "6" * 64)

    def test_api_version_must_still_match_requested_constraint(self):
        with mock.patch.object(StorageClient, "get_component_response", return_value=response("0.7.0")), \
                mock.patch.object(APIClient, "get_component_response", return_value=response("0.7.1")):
            self.assertFalse(self.client.versions(NAME, "^0.7.2").versions)
            with self.assertRaises(VersionNotFound):
                self.client.component(NAME, "0.7.2")

    def test_custom_mirrors_never_fall_back_to_public_registry(self):
        client = StorageClient("https://private.example/components")
        with mock.patch.object(StorageClient, "get_component_response", side_effect=ComponentNotFound()), \
                mock.patch.object(APIClient, "get_component_response") as api:
            with self.assertRaises(ComponentNotFound):
                client.versions(NAME, "^0.7.2")
            with self.assertRaises(ComponentNotFound):
                client.component(NAME, "0.7.2")
        api.assert_not_called()


if __name__ == "__main__":
    unittest.main()
