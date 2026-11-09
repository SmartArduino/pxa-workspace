import base64
import copy
import hashlib
import json
from types import SimpleNamespace
import unittest

from ui_query import UiQueryError, decode_snapshot, expect, tap_target, validate_snapshot


def node(identity, kind, *, parent=0, text=None, visible=True, enabled=True, clickable=False):
    return dict(surface=1, id=identity, parent=parent, type=kind, text=text,
                visible=visible, enabled=enabled, clickable=clickable,
                text_redacted=False, text_truncated=False,
                bounds=dict(x=20, y=30, width=100, height=40),
                clip=dict(x=20, y=30, width=100, height=40))


class UiQueryTests(unittest.TestCase):
    def setUp(self):
        self.tree = dict(schema='pxa-ui-inspection-1', renderer='lvgl', complete=True,
                         viewport=dict(width=296, height=240), nodes=[
                             node(1, 'root'), node(2, 'button', parent=1, clickable=True),
                             node(3, 'text', parent=2, text='开始 "中文"\n🙂'),
                             node(4, 'progress', parent=1)])
        self.tree['nodes'][3]['value'] = 0

    def test_chunk_integrity_and_real_snapshot_validation(self):
        raw = json.dumps(self.tree, ensure_ascii=False).encode()
        frame = lambda kind, payload: SimpleNamespace(kind=kind, payload=payload)
        frames = [frame('META', f'format=json;bytes={len(raw)};sha256={hashlib.sha256(raw).hexdigest()}')]
        for offset in range(0, len(raw), 77):
            frames.append(frame('DATA', f'{offset}\t' + base64.b64encode(raw[offset:offset+77]).decode()))
        frames.append(frame('OK', ''))
        self.assertEqual(decode_snapshot(frames), self.tree)
        for invalid in (frames[1:], frames[:-2], frames + [frames[1]]):
            with self.assertRaises(UiQueryError):
                decode_snapshot(invalid)

    def test_typed_button_uses_actual_descendant_and_text_tap_resolves_parent(self):
        label = self.tree['nodes'][2]['text']
        self.assertTrue(expect(self.tree, {'type': 'button', 'text': label}, {'enabled': True})['passed'])
        result = tap_target(self.tree, {'text': label})
        self.assertEqual((result['node']['id'], result['x'], result['y']), (2, 69, 49))
        self.tree['nodes'][1]['clip']['width'] = 20
        self.assertEqual(tap_target(self.tree, {'text': label})['x'], 29)

    def test_value_zero_disabled_hidden_and_outside_geometry_failures(self):
        self.assertTrue(expect(self.tree, {'type': 'progress'}, {'value': 0})['passed'])
        with self.assertRaises(UiQueryError):
            expect(self.tree, {'type': 'progress'}, {'value': 1})
        with self.assertRaises(UiQueryError):
            expect(self.tree, {'type': 'button'}, {'within': [0, 0, 30, 30]})
        for flag in ('enabled', 'visible'):
            tree = copy.deepcopy(self.tree)
            tree['nodes'][1][flag] = False
            tree['nodes'][2][flag] = False
            with self.assertRaises(UiQueryError):
                tap_target(tree, {'type': 'button'})
            self.assertTrue(expect(tree, {'type': 'button'}, {flag: False})['passed'])
        self.assertTrue(expect(self.tree, {'text': 'Missing'}, {'count': 0})['passed'])

    def test_ambiguous_matches_are_not_silently_clicked(self):
        other = copy.deepcopy(self.tree['nodes'][1]); other['id'] = 5
        self.tree['nodes'].append(other)
        with self.assertRaises(UiQueryError):
            tap_target(self.tree, {'type': 'button'})
        with self.assertRaises(UiQueryError):
            expect(self.tree, {'type': 'button'}, {})

    def test_malformed_graph_and_redacted_or_truncated_text(self):
        for change in ('cycle', 'duplicate', 'parent', 'password', 'partial'):
            tree = copy.deepcopy(self.tree)
            if change == 'cycle': tree['nodes'][0]['parent'] = 3
            if change == 'duplicate': tree['nodes'].append(tree['nodes'][1])
            if change == 'parent': tree['nodes'][1]['parent'] = 999
            if change == 'password': tree['nodes'][2]['text_redacted'] = True
            if change == 'partial': tree['complete'] = False
            with self.assertRaises(UiQueryError):
                validate_snapshot(tree)
        self.tree['nodes'][2]['text_truncated'] = True
        with self.assertRaises(UiQueryError):
            expect(self.tree, {'text': self.tree['nodes'][2]['text']}, {})


if __name__ == '__main__':
    unittest.main()
