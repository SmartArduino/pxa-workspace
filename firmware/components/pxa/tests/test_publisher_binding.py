"""Cross-repository server-to-native signature/wire integration, no device I/O."""
import argparse
import base64
import hashlib
import json
from dataclasses import replace
from datetime import datetime, timedelta, timezone
from pathlib import Path
import subprocess
import sys
import tempfile
from cryptography.hazmat.primitives import hashes,serialization
from cryptography.hazmat.primitives.asymmetric import ec,utils

p=argparse.ArgumentParser();p.add_argument('--server-root',type=Path,required=True);args=p.parse_args()
sys.path.insert(0,str(args.server_root.resolve()))
from app import catalog
root=Path(__file__).resolve().parents[4]
with tempfile.TemporaryDirectory(prefix='pxa-binding-') as temp:
    temp=Path(temp);key=ec.generate_private_key(ec.SECP256R1());private=temp/'issuer.pem';public=temp/'issuer.der'
    private.write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    public.write_bytes(key.public_key().public_bytes(serialization.Encoding.DER,serialization.PublicFormat.SubjectPublicKeyInfo))
    catalog.settings=replace(catalog.settings,catalog_signing_key_path=str(private));catalog._catalog_key.cache_clear()
    now=datetime.now(timezone.utc)
    payload={'schema':'pxa.publisher-binding/1','publisher_id':'39230638-1d80-4155-9db5-0ca4c3a093ab',
        'handle':'lucinhu','display_name':'作者 Lucin','revision':'9007199254740993','status':'active','app_id':'pxa-nes','lineage_root_key_id':'1'*64,
        'issued_at':now.strftime('%Y-%m-%dT%H:%M:%SZ'),'expires_at':(now+timedelta(days=30)).strftime('%Y-%m-%dT%H:%M:%SZ')}
    encoded=base64.b64decode(catalog.sign_publisher_binding(payload)['binding_token'],validate=True)
    token=temp/'binding.bin';token.write_bytes(encoded)
    core=root/'deps/pxa-system/libpxa';native=root/'firmware/components/pxa/src/package';tests=root/'firmware/components/pxa/tests'
    cflags=subprocess.check_output(['pkg-config','--cflags','libcjson'],text=True).split()
    libs=subprocess.check_output(['pkg-config','--libs','libcjson'],text=True).split()
    executable=temp/'verify'
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(native),'-I'+str(core/'include'),'-I'+str(core/'adapters/include'),*cflags,
        str(tests/'pxa_publisher_binding_test.c'),str(native/'pxa_publisher_binding.c'),str(native/'pxa_publisher_profile.c'),str(core/'adapters/openssl/pxa_openssl.c'),str(core/'src/core/wire.c'),'-lcrypto',*libs,'-o',str(executable)],check=True)
    subprocess.run([str(executable),str(token),str(public)],check=True)
    authority=temp/'authority.json';policy=temp/'issuer.c'
    authority.write_text(json.dumps({'algorithm':'ecdsa-p256-sha256-p1363','key_id':hashlib.sha256(public.read_bytes()).hexdigest(),'spki':base64.b64encode(public.read_bytes()).decode()}))
    subprocess.run([sys.executable,str(root/'tools/namespace-issuer.py'),str(authority),'--output',str(policy)],check=True)
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'firmware/components/pxa/include'),'-c',str(policy),'-o',str(temp/'issuer.o')],check=True)
    # An otherwise valid signature made in another protocol must be rejected.
    for domain in (catalog.PUBLISHER_BINDING_DOMAIN,catalog.CATALOG_DOMAIN):
        signature=key.sign(domain+encoded[:-64],ec.ECDSA(hashes.SHA256()))
        r,s=utils.decode_dss_signature(signature);s=min(s,catalog.P256_ORDER-s)
        token.write_bytes(encoded[:-64]+r.to_bytes(32,'big')+s.to_bytes(32,'big'))
        subprocess.run([str(executable),str(token),str(public),'reject'],check=True)
    print('PASS: JSON binding/catalog domains cannot be replayed as native binary proof')
