"""Queries over real, bounded Host UI snapshots; no OCR or Guest state access."""
from __future__ import annotations

import base64
import hashlib
import json

MAX_BYTES = 1024 * 1024
TYPES = {'root', 'box', 'scroll', 'text', 'image', 'button', 'toggle', 'slider',
         'text-input', 'select', 'progress', 'canvas', 'list', 'media-surface', 'unknown'}


class UiQueryError(ValueError):
    pass


def decode_snapshot(frames):
    metadata = [frame.payload for frame in frames if frame.kind == 'META']
    if len(metadata) != 1:
        raise UiQueryError('UI inspection requires exactly one META frame')
    fields = dict(part.split('=', 1) for part in metadata[0].split(';') if '=' in part)
    try:
        size = int(fields['bytes'])
        if fields['format'] != 'json' or not 0 < size <= MAX_BYTES:
            raise ValueError()
        data = bytearray()
        for frame in frames:
            if frame.kind != 'DATA':
                continue
            offset, encoded = frame.payload.split('\t', 1)
            chunk = base64.b64decode(encoded, validate=True)
            if int(offset) != len(data) or not chunk or len(data) + len(chunk) > size:
                raise ValueError()
            data.extend(chunk)
        if len(data) != size or hashlib.sha256(data).hexdigest() != fields['sha256']:
            raise ValueError()
        return validate_snapshot(json.loads(data))
    except (ValueError, KeyError, TypeError) as error:
        raise UiQueryError('Malformed, truncated or corrupt UI inspection') from error


def validate_snapshot(tree):
    if not isinstance(tree, dict) or tree.get('schema') != 'pxa-ui-inspection-1' or tree.get('complete') is not True:
        raise UiQueryError('Unsupported or incomplete UI inspection')
    viewport = tree.get('viewport', {})
    if not isinstance(viewport, dict) or any(type(viewport.get(key)) is not int or
            not 1 <= viewport[key] <= 65535 for key in ('width', 'height')):
        raise UiQueryError('Invalid UI viewport')
    nodes = tree.get('nodes')
    if not isinstance(nodes, list) or len(nodes) > 4096:
        raise UiQueryError('Invalid UI node count')
    index = {}
    for node in nodes:
        if not isinstance(node, dict) or any(type(node.get(k)) is not int or not 0 <= node[k] <= 0xffffffff
                for k in ('surface', 'id', 'parent')) or not node['surface'] or not node['id']:
            raise UiQueryError('Invalid UI node identity')
        key = node['surface'], node['id']
        if key in index or not isinstance(node.get('type'), str) or node['type'] not in TYPES:
            raise UiQueryError('Duplicate UI node or unsupported type')
        for flag in ('visible', 'enabled', 'clickable', 'text_redacted', 'text_truncated'):
            if type(node.get(flag)) is not bool:
                raise UiQueryError('Invalid UI node state')
        text = node.get('text')
        if text is not None and (not isinstance(text, str) or len(text.encode('utf-8')) > 4096):
            raise UiQueryError('Invalid UI text')
        if node['text_redacted'] and text is not None:
            raise UiQueryError('Host exposed a redacted text value')
        for name in ('bounds', 'clip'):
            rect = node.get(name)
            if not isinstance(rect, dict) or any(type(rect.get(k)) is not int or not -0x7fffffff <= rect[k] <= 0x7fffffff
                    for k in ('x', 'y', 'width', 'height')) or rect['width'] < 0 or rect['height'] < 0:
                raise UiQueryError('Invalid UI rectangle')
        if any(type(node[k]) is not int for k in ('value', 'minimum', 'maximum') if k in node):
            raise UiQueryError('Invalid UI value')
        index[key] = node
    visited = set()
    for key in index:
        chain = set()
        while key not in visited:
            if key in chain or key not in index:
                raise UiQueryError('Cyclic or missing UI parent')
            chain.add(key)
            node = index[key]
            if not node['parent']:
                break
            key = node['surface'], node['parent']
        visited.update(chain)
    return tree


def validate_selector(selector):
    if not isinstance(selector, dict) or not selector or set(selector) - {'id', 'surface', 'type', 'text'}:
        raise UiQueryError('Select by id, surface, type or exact text')
    if any(type(selector[k]) is not int or not 1 <= selector[k] <= 0xffffffff for k in ('id', 'surface') if k in selector):
        raise UiQueryError('Node and surface IDs must be positive integers')
    if 'type' in selector and (not isinstance(selector['type'], str) or selector['type'] not in TYPES):
        raise UiQueryError('Unsupported UI node type')
    if 'text' in selector and (not isinstance(selector['text'], str) or len(selector['text'].encode('utf-8')) > 4096):
        raise UiQueryError('UI selector text must be a bounded string')
    return selector


def validate_expectation(expected):
    if not isinstance(expected, dict) or set(expected) - {'visible', 'enabled', 'value', 'count', 'within'}:
        raise UiQueryError('Invalid UI expectation fields')
    if any(type(expected[k]) is not bool for k in ('visible', 'enabled') if k in expected):
        raise UiQueryError('Visibility and enabled expectations must be boolean')
    if 'count' in expected and (type(expected['count']) is not int or not 0 <= expected['count'] <= 4096):
        raise UiQueryError('UI expected count must be 0..4096')
    if 'value' in expected and type(expected['value']) is not int:
        raise UiQueryError('UI expected value must be an integer')
    if 'within' in expected:
        rect = expected['within']
        if not isinstance(rect, list) or len(rect) != 4 or any(type(v) is not int for v in rect) or min(rect[2:]) < 0:
            raise UiQueryError('Within requires [x,y,width,height]')
    return expected


def select(tree, selector):
    validate_selector(selector)
    index = {(node['surface'], node['id']): node for node in tree['nodes']}

    def has_text(node):
        if node['text'] == selector['text'] and not node['text_truncated']:
            return True
        # A typed button may own a separate Guest text node. Never aggregate a
        # whole page into a false text match; untyped text selects actual text.
        if selector.get('type') not in {'button', 'toggle', 'select'}:
            return False
        for child in tree['nodes']:
            if child['text'] != selector['text'] or child['text_truncated']:
                continue
            parent = child
            while parent['parent']:
                parent = index[parent['surface'], parent['parent']]
                if parent is node:
                    return True
        return False

    return [node for node in tree['nodes'] if all(
        has_text(node) if key == 'text' else node[key] == value for key, value in selector.items())]


def brief(node):
    return {key: (node[key][:160] if key == 'text' and isinstance(node[key], str) else node[key])
            for key in ('surface', 'id', 'type', 'text', 'visible', 'enabled', 'clickable', 'value', 'bounds', 'clip') if key in node}


def expect(tree, selector, expected):
    validate_expectation(expected)
    expected = {'visible': True, **expected}
    candidates = select(tree, selector)

    def matches(node):
        for key, value in expected.items():
            if key == 'count':
                continue
            if key == 'within':
                r = node['bounds']; x, y, w, h = value
                if not (r['x'] >= x and r['y'] >= y and r['x'] + r['width'] <= x + w and r['y'] + r['height'] <= y + h):
                    return False
            elif node.get(key) != value:
                return False
        return True

    matched = [node for node in candidates if matches(node)]
    count = expected.get('count', 1)
    if len(matched) != count:
        detail = {'selector': selector, 'expected': expected, 'matched_count': len(matched),
                  'candidate_count': len(candidates), 'candidates': [brief(n) for n in candidates[:8]]}
        raise UiQueryError('UI assertion failed: ' + json.dumps(detail, ensure_ascii=False))
    return {'passed': True, 'selector': selector, 'expected': expected,
            'matched_count': len(matched), 'nodes': [brief(n) for n in matched[:16]]}


def tap_target(tree, selector):
    candidates = select(tree, selector)
    index = {(node['surface'], node['id']): node for node in tree['nodes']}
    targets = {}
    for node in candidates:
        if not node['visible'] or not node['enabled']:
            continue
        while not node['clickable'] and node['parent']:
            node = index[node['surface'], node['parent']]
        if node['visible'] and node['enabled'] and node['clickable']:
            targets[node['surface'], node['id']] = node
    if len(targets) != 1:
        raise UiQueryError(f'UI tap requires one visible enabled target; found {len(targets)}: ' +
                           json.dumps([brief(n) for n in list(targets.values())[:8]], ensure_ascii=False))
    node = next(iter(targets.values()))
    clip = node['clip']
    if not clip['width'] or not clip['height']:
        raise UiQueryError('UI target has no visible input area')
    x, y = clip['x'] + (clip['width'] - 1) // 2, clip['y'] + (clip['height'] - 1) // 2
    return {'node': brief(node), 'x': x, 'y': y,
            'note': 'Pointer injection uses resolved geometry; verify the resulting state. Overlays may intercept input.'}
