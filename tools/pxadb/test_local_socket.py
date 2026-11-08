"""Exercise the same local socket transport used by native Windows PXADB."""
from pathlib import Path
import socketserver
import tempfile
import threading
import unittest

from pxa_local_socket import LocalServer, LocalSocket


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.sendall(self.request.recv(64))


class LocalSocketTest(unittest.TestCase):
    def test_bind_connect_and_io_timeout_without_tcp(self):
        with tempfile.TemporaryDirectory(prefix='pxa-sock-') as td:
            path = str(Path(td) / 'control.sock')
            try:
                server = LocalServer(path, Echo)
            except OSError as error:
                # Wine currently lacks WinSock AF_UNIX. Real supported Windows
                # versions must run this test, rather than using a TCP fallback.
                import os
                if os.name == 'nt' and getattr(error, 'winerror', None) == 10047:
                    import ctypes
                    if getattr(ctypes.WinDLL('ntdll'), 'wine_get_version', None):
                        self.skipTest('Wine does not implement WinSock AF_UNIX')
                raise
            with server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    for message in (b'PING\n', b'KEY_DOWN ENTER\n'):
                        with LocalSocket() as client:
                            client.settimeout(2)
                            client.connect(path)
                            self.assertEqual(client.gettimeout(), 2)
                            client.sendall(message)
                            self.assertEqual(client.recv(64), message)
                finally:
                    server.shutdown()
                    thread.join(timeout=5)
            Path(path).unlink(missing_ok=True)


if __name__ == '__main__':
    unittest.main()
