# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
"""TLS 1.2 server that requests a client certificate only by renegotiating (as some servers on
port 443 do). Per connection: handshake without a certificate request; on a line "RENEG",
renegotiate requesting one, then answer "client certificate: <CN>" or "no client certificate".

    reneg_server.py <port> <server.crt> <server.key> <ca.crt>
"""
import select
import socket
import sys

from cryptography.x509.oid import NameOID
from OpenSSL import SSL


def _wait(sock):
    select.select([sock], [], [], 5)


def _serve(ctx, sock):
    conn = SSL.Connection(ctx, sock)
    conn.set_accept_state()
    sock.setblocking(False)
    line = b""
    while not line.endswith(b"\n"):
        try:
            line += conn.recv(64)
        except SSL.WantReadError:
            _wait(sock)
    if line.strip() != b"RENEG":
        return
    conn.set_verify(SSL.VERIFY_PEER, lambda *args: True)
    conn.renegotiate()
    while True:
        try:
            conn.do_handshake()  # Sends the HelloRequest.
            break
        except SSL.WantReadError:
            _wait(sock)
    while conn.renegotiate_pending():
        try:
            conn.recv(1)  # Runs the client's handshake.
        except SSL.WantReadError:
            _wait(sock)
    peer = conn.get_peer_certificate(as_cryptography=True)
    answer = "no client certificate"
    if peer is not None:
        cn = peer.subject.get_attributes_for_oid(NameOID.COMMON_NAME)[0].value
        answer = "client certificate: " + cn
    conn.sendall((answer + "\n").encode())
    conn.shutdown()


def main():
    port, cert, key, ca = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
    ctx = SSL.Context(SSL.TLSv1_2_METHOD)
    ctx.use_certificate_file(cert)
    ctx.use_privatekey_file(key)
    ctx.load_verify_locations(ca)
    ctx.set_options(SSL.OP_NO_TICKET)  # A full handshake when renegotiating: no resumption.
    ctx.set_session_cache_mode(SSL.SESS_CACHE_OFF)
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(4)
    while True:
        sock, _ = listener.accept()
        try:
            _serve(ctx, sock)
        except (SSL.Error, OSError) as e:
            print("connection failed:", e, flush=True)
        finally:
            sock.close()


if __name__ == "__main__":
    main()
