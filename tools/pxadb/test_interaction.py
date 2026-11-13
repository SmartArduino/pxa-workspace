import json
import struct
import unittest
from types import SimpleNamespace
from unittest import mock
import pxadb
from package_compatibility import requirements
from test_package_compatibility import manifest, record


def device():
    return {'schema':'pxa-device-interaction-1',
        'identity':{name:'test' for name in ('vendor','model','hardware_revision','firmware_id','firmware_version','build_id')},
        'profile':{'id':'three-key','revision':1},'display':{'width':240,'height':284,'shape':'rounded'},
        'physical':{'touch':False,'max_contacts':0,'simultaneous':None,'controls':[{'id':'power','label':'BOOT'}]},
        'modes':[{'id':'focus','kind':'focus','actions':{'confirm':{'control':'power','events':['tap']}}}],
        'reserved':[]}


class InteractionTests(unittest.TestCase):
    def test_legacy_never_sends_unsupported_command(self):
        client=mock.Mock(device_info='capabilities=info,memory')
        self.assertEqual(pxadb.interaction_profile(client)['status'],'unknown')
        client.request.assert_not_called()

    def test_bounded_chunks_round_trip_and_hash(self):
        document=json.dumps(device(),ensure_ascii=False).encode()
        frames=[pxadb.Frame(1,'DATA','hex='+document[i:i+128].hex()) for i in range(0,len(document),128)]
        frames.append(pxadb.Frame(1,'OK',f'schema=1;bytes={len(document)}'))
        client=mock.Mock(device_info='capabilities=interaction-info');client.request.return_value=frames
        result=pxadb.interaction_profile(client)
        self.assertEqual(result['descriptor'],device());self.assertEqual(len(result['profile_hash']),64)
        self.assertFalse(result['verified']);client.request.assert_called_once_with('INTERACTION INFO')
        for bad in [frames[1:], [pxadb.Frame(1,'OK','schema=1;bytes=99999')],
                    [pxadb.Frame(1,'OK','schema=0;schema=1')]]:
            client.request.return_value=bad
            with self.assertRaises(ValueError):pxadb.interaction_profile(client)

    def test_manifest_reads_optional_signed_requirements(self):
        a={'schema':'pxa-app-interaction-1','profiles':[{'id':'focus','kind':'focus','complete':True,'requires':{'confirm':'tap'}}]}
        data=manifest();body=data[12:]+record(0x8015,json.dumps(a).encode())
        self.assertEqual(requirements(data[:8]+struct.pack('<I',len(body))+body)['interaction'],a)

    def test_explicit_mz79_profile_sets_control_lines_before_open(self):
        port=SimpleNamespace(dtr=False,rts=True,port=None,open=mock.Mock(),close=mock.Mock(),fileno=lambda:42)
        def opened():
            self.assertTrue(port.dtr);self.assertFalse(port.rts);self.assertEqual(port.port,'/dev/ttyACM2')
        port.open.side_effect=opened
        with mock.patch.object(pxadb,'serial_upload_profile',return_value='usb'), \
             mock.patch.object(pxadb,'_hold_usb_awake',return_value=None), \
             mock.patch.object(pxadb.serial,'Serial',return_value=port) as constructor, \
             mock.patch.dict(pxadb.os.environ,{'PXADB_SERIAL_PROFILE':'mz79'}), \
             mock.patch.object(pxadb.sys,'platform','win32'):
            pxadb.PxaDbClient('/dev/ttyACM2')
        self.assertIsNone(constructor.call_args.args[0]);port.open.assert_called_once()


if __name__=='__main__':unittest.main()
