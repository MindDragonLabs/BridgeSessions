"""Hermetic executable/HTTP-context regressions; no provider inference is performed."""
from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bridgepanel import api, auth, v1
from bridgepanel.server import BridgePanelHandler as Handler


class TestHeadlessAdapters(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.home = self.root / 'adapter-home'
        self.home.mkdir()
        self.executable = self.root / 'hermetic-executable'
        self.executable.write_text('#!' + sys.executable + '\n' + '''import json, os, sys, time
prompt = sys.argv[-1]
if prompt == 'inspect':
    print(json.dumps({'argv': sys.argv[1:], 'env': dict(os.environ), 'cwd': os.getcwd()}))
elif prompt.startswith('fork:'):
    if os.fork() == 0:
        while True:
            with open(prompt[5:], 'a') as stream: stream.write('tick\\n')
            time.sleep(.01)
    time.sleep(20)
elif prompt == 'sleep':
    time.sleep(20)
elif prompt == 'flood':
    print('x' * 100000)
elif prompt == 'fail':
    print('private stderr', file=sys.stderr)
    sys.exit(3)
else:
    print('executable fixture: ' + prompt)
''')
        self.executable.chmod(0o700)
        self.env = mock.patch.dict(os.environ, {
            'BRIDGEPANEL_CONFIG': str(self.root / 'panel-config'),
            'BRIDGEPANEL_AGENT_ADAPTERS': 'codex',
            'BRIDGEPANEL_AGENT_CODEX_EXECUTABLE': str(self.executable),
            'BRIDGEPANEL_AGENT_CODEX_HOME': str(self.home),
            'BRIDGEPANEL_AGENT_FIXTURES': '0',
            'HTTP_API_TOKEN': 'isolation-sentinel',
            'OPENAI_API_KEY': 'isolation-sentinel',
        })
        self.env.start()

    def tearDown(self):
        with v1._chat_lock:
            keys = list(v1._chat)
        for owner, request_id in keys:
            try:
                v1.chat_delete(request_id, owner)
            except v1.V1Error:
                pass
        with v1._chat_lock:
            futures = [row['_future'] for row in v1._chat.values()]
        for future in futures:
            if not future.cancelled():
                future.result(timeout=6)
        with v1._chat_lock:
            v1._chat.clear()
        self.env.stop()
        self.tmp.cleanup()

    def post(self, prompt, request_id='request', owner='device-a'):
        return v1.chat_post({'machine': '.', 'agent': 'codex', 'prompt': prompt,
                             'request_id': request_id}, owner)

    def wait_status(self, request_id='request', terminal=True):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            row = v1.chat_get(request_id, 'device-a')[1]
            if row['status'] in v1.TERMINAL if terminal else row['status'] == 'running':
                return row
            time.sleep(.01)
        self.fail('executable fixture did not reach expected status')

    def test_exact_contract_isolation_and_terminal_delete(self):
        self.assertEqual(self.post('inspect')[0], 202)
        row = self.wait_status()
        self.assertEqual(row['status'], 'completed')
        out = json.loads(row['result']['text'])
        self.assertEqual(out['argv'], ['exec', '--skip-git-repo-check', 'inspect'])
        self.assertEqual(out['env']['HOME'], str(self.home))
        self.assertNotIn('OPENAI_API_KEY', out['env'])
        self.assertNotIn('HTTP_API_TOKEN', out['env'])
        self.assertNotEqual(out['cwd'], os.getcwd())
        self.assertNotIn('_future', row)
        self.assertEqual(v1.chat_delete('request', 'device-a')[1]['status'], 'completed')

    def test_ownership_payload_conflict_and_remote_rejection(self):
        self.post('hello')
        self.wait_status()
        self.assertEqual(self.post('hello')[0], 200)
        with self.assertRaises(v1.V1Error) as conflict:
            self.post('changed')
        self.assertEqual(conflict.exception.status, 409)
        for action in (v1.chat_get, v1.chat_delete):
            with self.assertRaises(v1.V1Error) as denied:
                action('request', 'device-b')
            self.assertEqual(denied.exception.status, 404)
        self.post('other device', owner='device-b')
        with self.assertRaises(v1.V1Error) as remote:
            v1.chat_post({'machine': 'remote', 'agent': 'codex', 'prompt': 'x', 'request_id': 'remote'}, 'device-a')
        self.assertEqual(remote.exception.code, 'unsupported_machine')
        self.assertEqual(v1.dispatch('POST', '/api/v1/chat', {}, {})[0], 401)

    def test_unimplemented_or_missing_executable_is_not_available(self):
        with mock.patch.dict(os.environ, {'BRIDGEPANEL_AGENT_ADAPTERS': 'invented,codex',
                                         'BRIDGEPANEL_AGENT_CODEX_EXECUTABLE': '/missing'}):
            self.assertFalse(any(row['available'] for row in v1.agents()[1]['agents']))
            with mock.patch.object(api, 'bs_ipc', return_value=''):
                self.assertFalse(v1.capabilities()[1]['routes']['chat'])
            with self.assertRaises(v1.V1Error):
                self.post('hello')

    def test_timeout_and_cancel_stop_subprocess_groups(self):
        marker = self.root / 'heartbeat'
        with mock.patch.object(v1, 'CHAT_TIMEOUT', .2):
            self.post('fork:' + str(marker))
            row = self.wait_status()
        self.assertEqual(row['result']['error']['code'], 'agent_timeout')
        size = marker.stat().st_size
        time.sleep(.08)
        self.assertEqual(marker.stat().st_size, size)
        self.post('fork:' + str(marker), 'cancel')
        self.wait_status('cancel', terminal=False)
        deadline = time.monotonic() + 3
        while marker.stat().st_size == size and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertEqual(v1.chat_delete('cancel', 'device-a')[1]['status'], 'cancelled')
        size = marker.stat().st_size
        time.sleep(.08)
        self.assertEqual(marker.stat().st_size, size)
        self.assertEqual(v1.chat_get('cancel', 'device-a')[1]['status'], 'cancelled')

    def test_output_and_result_storage_limits_and_expiry(self):
        self.post('flood')
        self.assertEqual(self.wait_status()['result']['error']['code'], 'output_too_large')
        self.post('fail', 'fail')
        self.assertEqual(self.wait_status('fail')['result']['error']['code'], 'agent_failed')
        self.assertEqual(v1.chat_delete('fail', 'device-a')[1]['status'], 'failed')
        with mock.patch.object(v1, 'CHAT_STORE_LIMIT', 2):
            with self.assertRaises(v1.V1Error) as busy:
                self.post('hello', 'third')
            self.assertEqual(busy.exception.status, 429)
        with mock.patch.object(v1, 'CHAT_RESULT_TTL', 0):
            with self.assertRaises(v1.V1Error) as expired:
                v1.chat_get('request', 'device-a')
            self.assertEqual(expired.exception.status, 404)

    def test_queue_limit_and_queued_cancellation(self):
        import threading
        with mock.patch.object(v1, '_chat_slots', threading.BoundedSemaphore(3)):
            self.post('sleep', 'one')
            self.post('sleep', 'two')
            self.wait_status('one', terminal=False)
            self.wait_status('two', terminal=False)
            self.post('sleep', 'queued')
            self.assertEqual(v1.chat_get('queued', 'device-a')[1]['status'], 'pending')
            with self.assertRaises(v1.V1Error) as busy:
                self.post('sleep', 'overflow')
            self.assertEqual(busy.exception.status, 429)
            self.assertEqual(v1.chat_delete('queued', 'device-a')[1]['status'], 'cancelled')
            for request_id in ('one', 'two'):
                v1.chat_delete(request_id, 'device-a')
            with v1._chat_lock:
                futures = [row['_future'] for row in v1._chat.values()]
            for future in futures:
                if not future.cancelled(): future.result(timeout=5)


class TestV1SecurityContext(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.env = mock.patch.dict(os.environ, {'BRIDGEPANEL_CONFIG': self.tmp.name})
        self.env.start()
        self.handler = object.__new__(Handler)
        self.handler.path = '/api/v1/chat'
        self.handler.server = __import__('types').SimpleNamespace(bridgepanel_token='fixture-admin-bearer')

    def tearDown(self):
        auth.SESSIONS.clear()
        self.env.stop()
        self.tmp.cleanup()

    def test_scoped_identity_and_no_cookie_escalation(self):
        row = auth.issue_api_token('phone', ['chat'])
        self.handler.headers = {'Authorization': 'Bearer ' + row['token']}
        self.assertIsNotNone(self.handler.authorized_path())
        self.assertEqual(self.handler.auth_identity, 'token:' + row['id'])
        self.assertTrue(self.handler._v1_scope_allowed('POST', '/api/v1/chat'))
        self.assertFalse(self.handler._v1_scope_allowed('POST', '/api/v1/auth/tokens'))
        auth.set_password('admin', 'fixture password long')
        cookie = 'bp_session=' + auth.new_session()
        self.handler.headers['Cookie'] = cookie
        self.assertIsNotNone(self.handler.authorized_path())
        self.assertNotIn('admin', self.handler.auth_scopes)
        self.handler.headers['Authorization'] = 'Bearer invalid'
        self.assertIsNone(self.handler.authorized_path())
        self.handler.headers = {'Cookie': cookie, 'Host': 'localhost:1234'}
        self.assertIsNotNone(self.handler.authorized_path())
        self.assertFalse(self.handler._cookie_csrf_allowed())
        for origin in ('http://localhost:9876', 'http://127.0.0.1:1234', 'https://localhost:1234', 'null'):
            self.handler.headers['Origin'] = origin
            self.assertFalse(self.handler._cookie_csrf_allowed())
        self.handler.headers['Origin'] = 'http://localhost:1234'
        self.assertTrue(self.handler._cookie_csrf_allowed())

    def test_http_dispatch_get_post_delete_receive_only_identity(self):
        row = auth.issue_api_token("phone", ["chat"])
        self.handler.headers = {"Authorization": "Bearer " + row["token"]}
        self.handler.send_json = mock.Mock()
        self.handler._read_json_body = mock.Mock(return_value={"fixture": "body"})
        for method in ("GET", "POST", "DELETE"):
            self.handler.path = "/api/v1/chat" if method == "POST" else "/api/v1/chat/request"
            with mock.patch.object(v1, "dispatch", return_value=(200, {"ok": True})) as dispatch:
                getattr(self.handler, "do_" + method)()
            self.assertEqual(dispatch.call_args.kwargs, {"identity": "token:" + row["id"]})
            self.assertNotIn(row["token"], repr(dispatch.call_args))

    def test_local_volume_symlink_escape_is_rejected(self):
        root = Path(self.tmp.name) / "volume"
        root.mkdir()
        outside = Path(self.tmp.name) / "outside-volume"
        outside.write_text("outside fixture")
        (root / "escape").symlink_to(outside)
        with mock.patch.object(api, "list_host_volumes", return_value={"volumes": [{"token": "test-volume", "os_path": str(root)}]}), mock.patch.object(api, "query_mesh_tree", return_value={}), mock.patch.object(api, "is_self_node", return_value=True):
            result = api.read_volume_file(".", "test-volume", "escape")
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"], "path_rejected")

    def test_file_traversal_and_symlink_fail_closed(self):
        for path in ('../escape', '%2e%2e/escape', '/etc/passwd'):
            with mock.patch.object(api, 'list_host_files') as listing:
                status, _ = v1.dispatch('GET', '/api/v1/files', {'machine': ['.'], 'path': [path]})
                self.assertEqual(status, 400)
                listing.assert_not_called()
        root = Path(self.tmp.name) / 'received'
        root.mkdir()
        outside = Path(self.tmp.name) / 'outside'
        outside.write_text('outside fixture')
        (root / 'escape').symlink_to(outside)
        with mock.patch('bridgepanel.files.receive_dir', return_value=root), mock.patch.object(api, 'is_self_node', return_value=True):
            status, _ = v1.dispatch('GET', '/api/v1/files/content', {'machine': ['.'], 'path': ['escape']})
        self.assertEqual(status, 404)


if __name__ == '__main__':
    unittest.main()
