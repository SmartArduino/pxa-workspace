"""AF_UNIX on POSIX and Windows 10 1803+, without a TCP listener.

CPython's Windows socket module does not encode sockaddr_un. WinSock supports
it natively; only bind/connect need a small ctypes adapter. Framing, timeouts
and I/O still use the standard socket implementation.
"""
from __future__ import annotations

import os
from pathlib import Path
import socket
import socketserver
import select
import struct
import tempfile

AF_UNIX = getattr(socket, 'AF_UNIX', 1)


def socket_root() -> Path:
    if os.name == 'nt':
        # Windows TEMP is already per-user. Keep the basename short enough
        # for sockaddr_un's 108-byte limit, including the instance selector.
        default = Path(tempfile.gettempdir()) / 'pxa'
    else:
        default = Path(tempfile.gettempdir()) / ('pxa-simulator-' + str(os.getuid()))
    return Path(os.environ.get('PXA_SIMULATOR_SOCKET_ROOT', default))


def _windows_address(connection, path, operation):
    import ctypes
    encoded = os.fsencode(path)
    if b'\0' in encoded or len(encoded) >= 108:
        raise ValueError('Local simulator socket path must be shorter than 108 UTF-8 bytes')
    address = struct.pack('H', AF_UNIX) + encoded.ljust(108, b'\0')
    api = ctypes.WinDLL('ws2_32')
    function = getattr(api, operation)
    function.argtypes = [ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
    function.restype = ctypes.c_int
    timeout = connection.gettimeout()
    if function(connection.fileno(), address, len(address)) == 0:
        return
    error = api.WSAGetLastError()
    if operation == 'connect' and error in (10035, 10036, 10037) and timeout:
        _, writable, exceptional = select.select([], [connection], [connection], timeout)
        if not writable and not exceptional:
            raise TimeoutError(f'Local simulator connect timed out: {path}')
        error = connection.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
        if not error:
            return
    raise OSError(error, f'WinSock {operation} failed: {path}')


class LocalSocket(socket.socket):
    def __init__(self):
        super().__init__(AF_UNIX, socket.SOCK_STREAM)

    def connect(self, address):
        if os.name == 'nt':
            return _windows_address(self, address, 'connect')
        return super().connect(str(address))


class LocalServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    address_family = AF_UNIX
    daemon_threads = True

    def server_bind(self):
        if os.name == 'nt':
            _windows_address(self.socket, self.server_address, 'bind')
        else:
            self.socket.bind(str(self.server_address))
