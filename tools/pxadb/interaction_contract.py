# SPDX-License-Identifier: MIT
"""PXA interaction contracts v1. Canonical source; vendor copies stay byte-identical.

Descriptors are device/author declarations, never evidence of physical validation.
Missing data is unknown. Matching cannot synthesize actions or combine input modes.
"""
import hashlib
import json
import re

DEVICE_SCHEMA = 'pxa-device-interaction-1'
APP_SCHEMA = 'pxa-app-interaction-1'
MANIFEST_TAG = 0x8015  # Optional record 21, covered by both package signatures.
MAX_DEVICE_BYTES = 8192
MAX_APP_BYTES = 4096
KINDS = ('focus', 'controller', 'touch', 'pointer')
EVENTS = ('tap', 'press', 'release', 'hold', 'repeat', 'double_tap')
ID = re.compile(r'[a-z][a-z0-9._-]{0,63}\Z')


def require(condition, message):
    if not condition:
        raise ValueError('Interaction contract: ' + message)


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def decode(data, maximum):
    require(isinstance(data, (bytes, str)), 'document must be JSON bytes or text')
    require(len(data.encode() if isinstance(data, str) else data) <= maximum, 'document exceeds byte limit')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'duplicate JSON property')
            result[key] = value
        return result
    try:
        value = json.loads(data, object_pairs_hook=unique,
                           parse_constant=lambda _: require(False, 'non-finite number'))
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError('Interaction contract: invalid JSON') from error
    require(isinstance(value, dict), 'document must be an object')
    return value


def identifier(value):
    require(isinstance(value, str) and ID.fullmatch(value), 'invalid identifier')


def integer(value, minimum=0, maximum=65535):
    require(type(value) is int and minimum <= value <= maximum, 'invalid integer')


def bounded(value, maximum=64):
    require(isinstance(value, str) and 0 < len(value) <= maximum
            and all(ord(c) >= 32 for c in value), 'invalid text')


def array(value, maximum, minimum=0):
    require(isinstance(value, list) and minimum <= len(value) <= maximum, 'invalid array')


def groups(value, names):
    array(value, 16)
    for group in value:
        array(group, 8, 2)
        require(all(isinstance(name, str) and name in names for name in group)
                and len(set(group)) == len(group), 'unknown or repeated group member')


def validate_device(value):
    require(isinstance(value, dict) and value.get('schema') == DEVICE_SCHEMA, 'unsupported device schema')
    require(len(canonical(value)) <= MAX_DEVICE_BYTES, 'device descriptor too large')
    identity, profile = value.get('identity'), value.get('profile')
    require(isinstance(identity, dict) and isinstance(profile, dict), 'missing device identity/profile')
    for key in ('vendor', 'model', 'hardware_revision', 'firmware_id', 'firmware_version', 'build_id'):
        bounded(identity.get(key), 128)
    identifier(profile.get('id')); integer(profile.get('revision'), 1, 2147483647)
    display = value.get('display')
    require(isinstance(display, dict), 'missing display')
    integer(display.get('width'), 1); integer(display.get('height'), 1)
    require(display.get('shape') in ('rectangle', 'rounded', 'circle', 'unknown'), 'invalid display shape')
    physical = value.get('physical')
    require(isinstance(physical, dict), 'missing physical input description')
    require(physical.get('touch') is None or type(physical['touch']) is bool, 'invalid touch state')
    if physical.get('max_contacts') is not None:
        integer(physical['max_contacts'], 0, 16)
        require(physical.get('touch') is not False or physical['max_contacts'] == 0, 'inconsistent touch state')
    require(physical.get('simultaneous') is None or type(physical['simultaneous']) is bool, 'invalid simultaneous state')
    controls = physical.get('controls'); array(controls, 32)
    for control in controls:
        require(isinstance(control, dict), 'invalid control')
        identifier(control.get('id')); bounded(control.get('label'))
    names = [item['id'] for item in controls]
    require(len(set(names)) == len(names), 'duplicate physical control')
    groups(physical.get('exclusive_groups', []), names)
    modes = value.get('modes'); array(modes, 16)
    mode_ids = set()
    for mode in modes:
        require(isinstance(mode, dict) and mode.get('kind') in KINDS, 'invalid input mode')
        identifier(mode.get('id')); require(mode['id'] not in mode_ids, 'duplicate mode'); mode_ids.add(mode['id'])
        actions = mode.get('actions')
        require(isinstance(actions, dict) and 1 <= len(actions) <= 32, 'invalid mode actions')
        for name, binding in actions.items():
            identifier(name); require(isinstance(binding, dict), 'invalid binding')
            require(binding.get('control') in names, 'binding has no physical control')
            events = binding.get('events'); array(events, 6, 1)
            require(all(event in EVENTS for event in events) and len(set(events)) == len(events), 'invalid binding events')
            if binding.get('hold_limit_ms') is not None:
                integer(binding['hold_limit_ms'], -1, 600000)
            if binding.get('tap_latency_ms') is not None:
                integer(binding['tap_latency_ms'], 0, 10000)
        if mode['kind'] == 'touch':
            require(physical.get('touch') is not False, 'touch mode on a device without touch')
    reserved = value.get('reserved', []); array(reserved, 32)
    for rule in reserved:
        require(isinstance(rule, dict) and rule.get('control') in names, 'invalid reserved control')
        require(rule.get('event') in EVENTS, 'invalid reserved gesture')
        identifier(rule.get('action'))
        if rule.get('threshold_ms') is not None:
            integer(rule['threshold_ms'], 1, 600000)
    # Extensions contain data only; no executable code or prompt instructions are interpreted.
    return value


def validate_app(value):
    require(isinstance(value, dict) and value.get('schema') == APP_SCHEMA, 'unsupported app schema')
    require(len(canonical(value)) <= MAX_APP_BYTES, 'app declaration too large')
    profiles = value.get('profiles'); array(profiles, 16, 1)
    seen = set()
    for profile in profiles:
        require(isinstance(profile, dict), 'invalid app profile')
        identifier(profile.get('id')); require(profile['id'] not in seen, 'duplicate app profile'); seen.add(profile['id'])
        require(profile.get('kind') in KINDS and profile.get('complete') is True, 'profile must declare a complete operation path')
        actions = profile.get('requires')
        require(isinstance(actions, dict) and 1 <= len(actions) <= 32, 'invalid required actions')
        for name, event in actions.items():
            identifier(name); require(event in EVENTS, 'invalid required event')
        groups(profile.get('simultaneous', []), actions)
        holds = profile.get('holds', {})
        require(isinstance(holds, dict) and set(holds) <= actions.keys(), 'unknown held action')
        for duration in holds.values():
            integer(duration, 0, 600000)  # Zero requires indefinitely held input.
        for key in ('min_width', 'min_height', 'max_latency_ms'):
            if key in profile:
                integer(profile[key], 1, 65535)
        if 'contacts' in profile:
            integer(profile['contacts'], 1, 16)
        require(profile.get('kind') == 'touch' or 'contacts' not in profile, 'contacts require a touch profile')
    return value


def descriptor_hash(value):
    static = {key: item for key, item in validate_device(value).items() if key != 'runtime'}
    return hashlib.sha256(canonical(static)).hexdigest()


def evaluate(app, device):
    if app is not None:
        validate_app(app)
    if device is not None:
        validate_device(device)
    missing = 'app_declaration_missing' if app is None else 'device_description_missing' if device is None else None
    if missing:
        return {'status': 'unknown', 'verified': False, 'reason': missing, 'profiles': []}
    reports = []
    physical = device['physical']
    for profile in app['profiles']:
        modes = [mode for mode in device['modes'] if mode['kind'] == profile['kind']]
        candidates = []
        for mode in modes:
            issues = []
            def issue(code, unknown=False, action=None):
                issues.append({'code': code, 'status': 'unknown' if unknown else 'incompatible',
                               **({'action': action} if action else {})})
            for axis in ('width', 'height'):
                if device['display'][axis] < profile.get('min_' + axis, 1):
                    issue('display_too_small')
            if profile['kind'] == 'touch':
                if physical.get('touch') is not True:
                    issue('touch_unavailable', physical.get('touch') is None)
                count = physical.get('max_contacts')
                if count is None or count < profile.get('contacts', 1):
                    issue('touch_contacts_insufficient', count is None)
            for action, event in profile['requires'].items():
                binding = mode['actions'].get(action)
                if binding is None or event not in binding['events']:
                    issue('action_unavailable', action=action); continue
                if 'max_latency_ms' in profile and event in ('tap', 'double_tap'):
                    latency = binding.get('tap_latency_ms')
                    if latency is None or latency > profile['max_latency_ms']:
                        issue('tap_latency', latency is None, action)
            for group in profile.get('simultaneous', []):
                bound = [mode['actions'][action]['control'] for action in group if action in mode['actions']]
                if len(bound) != len(group):
                    continue
                if (len(set(bound)) != len(bound) or physical.get('simultaneous') is False
                        or any(len(set(bound) & set(exclusive)) > 1 for exclusive in physical.get('exclusive_groups', []))):
                    issue('simultaneous_conflict')
                elif physical.get('simultaneous') is None:
                    issue('simultaneous_unknown', True)
            holds = {action: 0 for action, event in profile['requires'].items() if event == 'hold'}
            holds.update(profile.get('holds', {}))
            for action, duration in holds.items():
                binding = mode['actions'].get(action)
                if binding is None:
                    continue
                limit = binding.get('hold_limit_ms')
                if 'hold' not in binding['events'] or limit == 0:
                    issue('hold_unavailable', action=action)
                elif limit is None:
                    issue('hold_unknown', True, action)
                elif limit != -1 and (duration == 0 or duration > limit):
                    issue('hold_limit', action=action)
                for rule in device.get('reserved', []):
                    if rule['control'] == binding['control'] and rule['event'] == 'hold':
                        threshold = rule.get('threshold_ms')
                        if threshold is None:
                            issue('system_hold_unknown', True, action)
                        elif duration == 0 or duration >= threshold:
                            issue('system_hold_reserved', action=action)
            status = ('incompatible' if any(item['status'] == 'incompatible' for item in issues)
                      else 'unknown' if issues else 'compatible')
            candidates.append({'profile': profile['id'], 'mode': mode['id'], 'status': status, 'issues': issues})
        report = next((item for state in ('compatible', 'unknown', 'incompatible') for item in candidates if item['status'] == state),
                      {'profile': profile['id'], 'status': 'incompatible', 'issues': [{'code': 'mode_unavailable', 'status': 'incompatible'}]})
        reports.append(report)
    selected = next(item for state in ('compatible', 'unknown', 'incompatible') for item in reports if item['status'] == state)
    return {'status': selected['status'], 'verified': False, 'reason': 'device_and_author_declarations',
            'selected_profile': selected['profile'] if selected['status'] == 'compatible' else None,
            'device_profile_hash': descriptor_hash(device), 'profiles': reports}
