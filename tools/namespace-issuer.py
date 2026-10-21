#!/usr/bin/env python3
"""Generate a product-owned PXA namespace trust table from reviewed public keys."""
import argparse
import base64
import hashlib
import json
from pathlib import Path


def generate(value):
    records=value if isinstance(value,list) else [value]
    if not 1<=len(records)<=16:raise ValueError('expected 1..16 issuer keys')
    out=['/* Product-owned namespace trust policy; contains public keys only. */','#include "pxa/pxa_publisher_namespace.h"']
    entries=[];seen=set()
    for i,record in enumerate(records):
        if record.get('algorithm')!='ecdsa-p256-sha256-p1363':raise ValueError('unsupported issuer algorithm')
        spki=base64.b64decode(record['spki'],validate=True)
        # Canonical DER SubjectPublicKeyInfo for id-ecPublicKey/secp256r1.
        prefix=bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d03010703420004')
        digest=hashlib.sha256(spki).hexdigest()
        if len(spki)!=91 or not spki.startswith(prefix) or record['key_id']!=digest or digest in seen:raise ValueError('invalid, duplicate or mismatched issuer SPKI')
        seen.add(digest)
        authority=bytes.fromhex(record.get('authority_id',digest))
        if len(authority)!=32 or not any(authority):raise ValueError('authority_id must be a nonzero stable 32-byte ID')
        octets=lambda data:','.join('0x%02x'%b for b in data)
        out.append(f'static const uint8_t issuer_{i}[] = '+'{'+octets(spki)+'};')
        entries.append('{issuer_%d,sizeof(issuer_%d),{'%(i,i)+octets(authority)+'}}')
    out += ['static const PxaPublisherNamespaceIssuer issuers[] = {'+','.join(entries)+'};',
        'size_t pxa_platform_trusted_namespace_issuers(const PxaPublisherNamespaceIssuer **out) {',
        '    if(out)*out=issuers;','    return sizeof(issuers)/sizeof(issuers[0]);','}']
    return '\n'.join(out)+'\n'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('authority_json',type=Path,help='reviewed /api/v3/publishers/authority response or a list of pinned keys')
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    source=generate(json.loads(args.authority_json.read_text()))
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(source)


if __name__=='__main__':main()
