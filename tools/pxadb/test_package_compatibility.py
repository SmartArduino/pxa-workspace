"""Compatibility admission, legacy unknowns and pre-upload write barriers."""
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import Mock, patch

import pxadb
from package_compatibility import requirements, evaluate


def record(tag, data):
    return struct.pack('<HH',tag,len(data))+data


def manifest():
    service = (record(1,struct.pack('<H',3))+record(2,struct.pack('<HH',0,8))+
               record(3,struct.pack('<HH',0,65535))+record(4,struct.pack('<Q',2)))
    artifact = (record(1,b'\2')+record(2,b'artifacts/main.esp32-p4.aot')+
                record(3,b'esp32-p4')+record(4,b'wamr')+record(5,b'test-engine-abi')+
                record(6,bytes(8))+record(7,b'\1'))
    component = record(1,b'main')+record(2,b'\1')+record(3,b'\2')+record(4,artifact)+record(5,service)
    body = record(2,b'pxa-test-app')+record(7,struct.pack('<HH',2,1))+record(16,component)
    return b'PXAM'+struct.pack('<HHI',0,7,len(body))+body


def host():
    return {'core':[2,1], 'manifest_max':[0,8], 'target':'esp32-p4','engine':'wamr',
            'engine_abi':'test-engine-abi','artifact_kinds':[2],'memory_model':1,
            'artifact_features':0,'execution_models':1,'services_complete':True,
            'services':[{'id':3,'version':[0,8],'features':2}], 'ready':'ready'}


class CompatibilityTests(unittest.TestCase):
    def setUp(self):
        self.package = requirements(manifest())
        self.host = host()

    def test_actual_binary_requirements_and_release_numbers_are_independent(self):
        self.assertEqual(self.package['min_core'],[2,1])
        self.assertEqual(self.package['components'][0]['services'][0]['features'],2)
        self.host['firmware_release']='0.1.0'
        self.assertEqual(evaluate(self.package,self.host)['status'],'compatible')
        self.host['firmware_release']='99.0.0';self.host['core']=[2,0]
        report=evaluate(self.package,self.host)
        self.assertEqual(report['status'],'incompatible')
        self.assertFalse(report['signature_verified'])

    def test_service_versions_features_and_complete_vs_unknown_inventory(self):
        for values in ({'version':[0,7],'features':2},{'version':[0,8],'features':0}):
            self.host['services']=[dict(id=3,**values)]
            self.assertEqual(evaluate(self.package,self.host)['status'],'incompatible')
        self.host['services']=[]
        self.assertEqual(evaluate(self.package,self.host)['status'],'incompatible')
        self.host['services_complete']=False
        report=evaluate(self.package,self.host)
        self.assertEqual(report['status'],'unknown')
        with self.assertRaisesRegex(pxadb.PxaDbError,'No package was uploaded'):
            pxadb.compatibility_admission(report,strict=True)

    def test_artifact_alternatives_and_actual_main_loop_support(self):
        self.host['engine_abi']='different'
        self.assertEqual(evaluate(self.package,self.host)['status'],'incompatible')
        component=self.package['components'][0]
        component['artifacts'].append(dict(kind=1,path='main.wasm',memory_model=1,features=0))
        self.host['artifact_kinds']=[1,2]
        self.assertEqual(evaluate(self.package,self.host)['status'],'compatible')
        self.host['execution_models']=0
        self.assertEqual(evaluate(self.package,self.host)['status'],'incompatible')
        self.host['execution_models']=None
        self.assertEqual(evaluate(self.package,self.host)['status'],'unknown')

    def test_parser_rejects_duplicates_truncation_and_required_unknown_fields(self):
        for extra in (record(7,struct.pack('<HH',2,0)),record(0x007f,b'x'),b'x'):
            data=manifest()+extra
            data=data[:8]+struct.pack('<I',len(data)-12)+data[12:]
            with self.assertRaises(ValueError):requirements(data)

    def test_legacy_is_unknown_and_advertised_query_failures_propagate(self):
        client=Mock(device_info='target=esp32p4;package_stream=2;package_legacy=0;package_codecs=0,1,2;package_max_block=32768')
        client.request.return_value=[pxadb.Frame(1,'OK','target=esp32p4;pxa_core=2.1;pxa_version=0.9.0')]
        profile=pxadb.runtime_profile(client)
        self.assertEqual(profile['target'],'esp32-p4')
        self.assertFalse(profile['services_complete'])
        self.assertEqual(evaluate(self.package,profile)['status'],'unknown')
        client.request.assert_called_once_with('INFO')
        client.device_info+=';capabilities=runtime-info'
        client.request.side_effect=[client.request.return_value,pxadb.PxaDbError('transport timed out')]
        with self.assertRaisesRegex(pxadb.PxaDbError,'transport timed out'):
            pxadb.runtime_profile(client)

    def test_runtime_inventory_integrity_and_unknown_feature_fields(self):
        client=Mock(device_info='target=esp32p4;capabilities=runtime-info')
        header='schema=1;core=2.1;target=esp32-p4;engine=wamr;engine_abi=test-engine-abi;memory_model=1;artifact_kinds=2;artifact_features=0;execution_models=1;manifest_max=0.8;service_count=1'
        service=pxadb.Frame(1,'DATA','id=3;version=0.8;features=unknown')
        client.request.side_effect=[[pxadb.Frame(1,'OK','pxa_core=2.1')],[service,pxadb.Frame(1,'OK',header)]]
        profile=pxadb.runtime_profile(client)
        self.assertEqual(evaluate(self.package,profile)['status'],'unknown')
        client.request.side_effect=[[pxadb.Frame(1,'OK','pxa_core=2.1')],[service,service,pxadb.Frame(1,'OK',header)]]
        with self.assertRaisesRegex(ValueError,'duplicate runtime service'):pxadb.runtime_profile(client)
        client.request.side_effect=[[pxadb.Frame(1,'OK','pxa_core=2.1')],[pxadb.Frame(1,'OK',header)]]
        with self.assertRaisesRegex(ValueError,'Incomplete runtime service inventory'):pxadb.runtime_profile(client)
        client.request.side_effect=[[pxadb.Frame(1,'OK','pxa_core=2.1')],[service,pxadb.Frame(1,'OK',header+';core=2.0')]]
        with self.assertRaisesRegex(ValueError,'duplicate runtime capability field'):pxadb.runtime_profile(client)

    def test_known_incompatible_local_package_never_reaches_any_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory);(source/'manifest.pxm').write_bytes(manifest());(source/'signature.pxs').touch()
            arguments=pxadb.build_parser().parse_args(['package','install',str(source),'--port','/dev/test','--yes'])
            client=Mock(device_info="");client.__enter__=Mock(return_value=client);client.__exit__=Mock(return_value=False)
            self.host['core']=[2,0]
            with patch.object(pxadb,'open_client',return_value=client), \
                 patch.object(pxadb,'runtime_profile',return_value=self.host), \
                 patch.object(pxadb,'existing_package_volume') as placement, \
                 patch.object(pxadb,'stage_package_files') as stage:
                with self.assertRaisesRegex(pxadb.PxaDbError,'Needs Core 2.1'):
                    pxadb.command_package_install(arguments)
                placement.assert_not_called();stage.assert_not_called()
                client.request.assert_not_called()

    def test_later_preflight_rejection_does_not_delete_preexisting_staged_data(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory);(source/'manifest.pxm').write_bytes(manifest());(source/'signature.pxs').touch()
            arguments=pxadb.build_parser().parse_args(['package','install',str(source),'--port','/dev/test','--yes'])
            client=Mock(device_info='');client.__enter__=Mock(return_value=client);client.__exit__=Mock(return_value=False)
            self.host['core']=[2,0]
            rejected=evaluate(self.package,self.host)
            with patch.object(pxadb,'open_client',return_value=client), \
                 patch.object(pxadb,'package_compatibility',side_effect=[{'status':'compatible','checks':[]},rejected]), \
                 patch.object(pxadb,'clear_staged_source') as cleanup:
                with self.assertRaisesRegex(pxadb.PxaDbError,'No package was uploaded'):
                    pxadb.command_package_install(arguments)
                cleanup.assert_not_called();client.request.assert_not_called()

    def test_strict_staged_identity_requires_a_local_manifest_before_opening_port(self):
        arguments=pxadb.build_parser().parse_args(['package','install','pxa-staged','--port','/dev/test','--strict-compatibility'])
        with patch.object(pxadb,'open_client') as client:
            with self.assertRaisesRegex(pxadb.PxaDbError,'requires the local package manifest'):
                pxadb.command_package_install(arguments)
            client.assert_not_called()


if __name__ == '__main__':unittest.main()
