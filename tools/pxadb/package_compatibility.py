"""Read-only package requirements and Host compatibility (no SDK-version guesses)."""
from __future__ import annotations

import struct


def records(data):
    offset = 0
    while offset < len(data):
        if offset + 4 > len(data):
            raise ValueError('Truncated compatibility manifest record')
        tag, size = struct.unpack_from('<HH', data, offset)
        offset += 4
        if offset + size > len(data):
            raise ValueError('Truncated compatibility manifest value')
        yield tag, data[offset:offset+size]
        offset += size


def fields(data, single, repeated=()):
    result = {}
    for raw_tag, value in records(data):
        tag = raw_tag & 0x7fff
        if tag not in single and tag not in repeated:
            if raw_tag & 0x8000:
                raise ValueError('Unknown required manifest field: ' + str(tag))
            continue
        if tag in repeated:
            result.setdefault(tag, []).append(value)
        elif tag in result:
            raise ValueError('Duplicate compatibility manifest field: ' + str(tag))
        else:
            result[tag] = value
    return result


def number(data, format):
    if data is None or len(data) != struct.calcsize(format):
        raise ValueError('Invalid numeric compatibility manifest field')
    values = struct.unpack(format, data)
    return list(values) if len(values) > 1 else values[0]


def text(data):
    value = data.decode('ascii')
    if not value or '\0' in value or len(value) > 256:
        raise ValueError('Invalid compatibility manifest identifier')
    return value


def requirements(manifest, container=None):
    """Decode the delivered binary declarations, including packager-added services."""
    if len(manifest) < 12 or len(manifest) > 16384 or manifest[:4] != b'PXAM':
        raise ValueError('Invalid compatibility manifest header/size')
    major, minor, size = struct.unpack_from('<HHI', manifest, 4)
    if size != len(manifest)-12:
        raise ValueError('Invalid compatibility manifest length')
    top = fields(manifest[12:], set(range(1,14)), (16,17,18,19,20))
    result = {'app_id': text(top[2]), 'manifest_version': [major,minor],
              'min_core': number(top[7], '<HH') if 7 in top else None,
              'container': container, 'components': []}
    component_ids = set()
    for raw in top.get(16, []):
        item = fields(raw, (1,2,3), (4,5))
        identity = text(item[1])
        if identity in component_ids:
            raise ValueError('Duplicate component in compatibility manifest')
        component_ids.add(identity)
        component = {'id': identity, 'flags': number(item.get(3,b'\0'), '<B'),
                     'artifacts': [], 'services': []}
        service_ids = set()
        for raw_service in item.get(5, []):
            service = fields(raw_service, (1,2,3,4))
            service_id = number(service.get(1), '<H')
            if not service_id or service_id in service_ids:
                raise ValueError('Invalid or duplicate service in compatibility manifest')
            service_ids.add(service_id)
            minimum, maximum = number(service.get(2), '<HH'), number(service.get(3), '<HH')
            if minimum[0] != maximum[0] or minimum > maximum:
                raise ValueError('Invalid service version range')
            component['services'].append({'id':service_id,'min':minimum,'max':maximum,
                'features':number(service.get(4), '<Q')})
        for raw_artifact in item.get(4, []):
            artifact = fields(raw_artifact, range(1,8))
            kind = number(artifact.get(1), '<B')
            if kind not in (1,2):
                raise ValueError('Unknown artifact kind in compatibility manifest')
            component['artifacts'].append({'kind':kind,'path':text(artifact[2]),
                'target':text(artifact[3]) if kind == 2 else None,
                'engine':text(artifact[4]) if kind == 2 else None,
                'engine_abi':text(artifact[5]) if kind == 2 else None,
                'features':number(artifact.get(6), '<Q'),
                'memory_model':number(artifact.get(7), '<B')})
        if not component['artifacts']:
            raise ValueError('Component has no execution artifact')
        result['components'].append(component)
    return result


def target_name(value):
    return {'esp32s3':'esp32-s3', 'esp32s31':'esp32-s31', 'esp32p4':'esp32-p4'}.get(value,value)


def evaluate(package, host):
    """Three-valued comparison. Missing telemetry never means compatible/absent."""
    checks = []
    def check(subject, required, actual, predicate, message):
        status = 'unknown' if actual is None else ('compatible' if predicate(actual) else 'incompatible')
        checks.append({'subject':subject,'status':status,'required':required,
                       'actual':actual,'message':message})
    if host.get('ready') is not None:
        check('runtime','ready',host['ready'],lambda state:state == 'ready','PXA runtime readiness')
    version = package['manifest_version']
    check('manifest', version, host.get('manifest_max'),
          lambda v: v[0] == version[0] and v[1] >= version[1], 'Manifest format support')
    core = package['min_core']
    if core is None:
        checks.append({'subject':'core','status':'unknown','required':None,
                       'actual':host.get('core'),'message':'Package does not declare its Core minimum'})
    else:
        check('core',core,host.get('core'),lambda v:v[0] == core[0] and v[1] >= core[1],
              f'Needs Core {core[0]}.{core[1]} or a later minor of that major')
    container = package.get('container')
    if container:
        version = container['version']
        check('container',version,host.get('container_versions'),lambda vs:version in vs,'Container format support')
        if version == [0,2]:
            check('codec',container['codec'],host.get('codecs'),lambda cs:container['codec'] in cs,'Container compression support')
            check('block_bytes',container['block_bytes'],host.get('max_block'),
                  lambda size:size >= container['block_bytes'],'Container decoding block limit')
    services = {service['id']:service for service in host.get('services',[])}
    for component in package['components']:
        prefix = 'component '+component['id']
        if component['flags'] & ~3:
            check(prefix+'/flags',component['flags'],0,lambda _:False,'Unknown required component flags')
        if component['flags'] & 2:
            check(prefix+'/main-loop',1,host.get('execution_models'),lambda modes:bool(modes & 1),
                  'Main-loop execution requires actual engine support')
        for service in component['services']:
            subject = prefix+'/service '+str(service['id'])
            provided = services.get(service['id'])
            if provided is None:
                check(subject,service,False if host.get('services_complete') else None,
                      lambda _:False,'Required service availability')
                continue
            low,high = service['min'],service['max']
            check(subject+'/version',{'min':low,'max':high},provided.get('version'),
                  lambda v:v[0] == low[0] and low <= v <= high,'Required service version range')
            if service['features']:
                check(subject+'/features',hex(service['features']),provided.get('features'),
                      lambda bits:service['features'] & ~bits == 0,'Required service feature bits')
        alternatives = []
        for artifact in component['artifacts']:
            comparisons = []
            def matches(actual, predicate):
                comparisons.append(None if actual is None else bool(predicate(actual)))
            matches(host.get('artifact_kinds'),lambda kinds:artifact['kind'] in kinds)
            matches(host.get('memory_model'),lambda model:model == artifact['memory_model'])
            if artifact['features']:
                matches(host.get('artifact_features'),lambda bits:artifact['features'] & ~bits == 0)
            if artifact['kind'] == 2:
                for field in ('target','engine','engine_abi'):
                    matches(host.get(field), lambda value,field=field: (
                        target_name(value) if field == 'target' else value) == artifact[field])
            state = 'incompatible' if False in comparisons else ('unknown' if None in comparisons else 'compatible')
            alternatives.append({'path':artifact['path'],'status':state,'requirements':artifact})
        states = [item['status'] for item in alternatives]
        state = 'compatible' if 'compatible' in states else ('unknown' if 'unknown' in states else 'incompatible')
        checks.append({'subject':prefix+'/artifact','status':state,'alternatives':alternatives,
                       'message':'Needs one executable artifact with matching CPU, memory model, engine and ABI; '
                                 f'Host target={host.get("target")}, engine_abi={host.get("engine_abi")}'})
    states = [item['status'] for item in checks]
    status = 'incompatible' if 'incompatible' in states else ('unknown' if 'unknown' in states else 'compatible')
    return {'schema':'pxa-package-compatibility-1','status':status,'package':package,'host':host,
            'checks':checks,'signature_verified':False,
            'note':'Read-only admission check. Device signature, permissions, storage and activation remain authoritative; release numbers alone do not determine compatibility.'}
