import argparse
import socket
import sys
import threading
import time

MODES = (
    "dribble",
    "tiny-chunks",
    "half-close",
    "headers-only",
    "garbage-prefix",
    "silent",
    "hangup",
    "wrong-length",
    "endless",
    "slow-headers",
)


def send_all(conn, payload):
    try:
        conn.sendall(payload)
        return True
    except OSError:
        return False


def read_request(conn):
    conn.settimeout(5.0)
    buffered = b""
    while b"\r\n\r\n" not in buffered:
        try:
            chunk = conn.recv(4096)
        except OSError:
            return None
        if not chunk:
            return None
        buffered += chunk
        if len(buffered) > 65536:
            return None
    return buffered


def dribble(conn):
    body = b"dribbled"
    response = (
        b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%s" % (len(body), body)
    )
    for index in range(len(response)):
        if not send_all(conn, response[index : index + 1]):
            return
        time.sleep(1.0)


def tiny_chunks(conn):
    body = b"one byte at a time"
    if not send_all(conn, b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"):
        return
    for index in range(len(body)):
        if not send_all(conn, b"1\r\n%s\r\n" % body[index : index + 1]):
            return
        time.sleep(0.01)
    send_all(conn, b"0\r\n\r\n")


def half_close(conn):
    send_all(conn, b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly ten b")
    try:
        conn.shutdown(socket.SHUT_WR)
    except OSError:
        pass
    time.sleep(0.2)


def headers_only(conn):
    send_all(conn, b"HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n")
    time.sleep(0.5)


def garbage_prefix(conn):
    send_all(conn, b"\x00\xff<html>not a status line</html>\r\n\r\n")
    time.sleep(0.2)


def silent(conn):
    time.sleep(10.0)


def hangup(conn):
    return


def wrong_length(conn):
    send_all(conn, b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfar more body than five bytes")
    time.sleep(0.2)


def endless(conn):
    send_all(conn, b"HTTP/1.1 200 OK\r\n")
    while True:
        if not send_all(conn, b"X-Pad: filler\r\n"):
            return


def slow_headers(conn):
    send_all(conn, b"HTTP/1.1 200 OK\r\n")
    for _ in range(20):
        if not send_all(conn, b"X-Slow"):
            return
        time.sleep(0.3)
        if not send_all(conn, b": value\r\n"):
            return


HANDLERS = {
    "dribble": dribble,
    "tiny-chunks": tiny_chunks,
    "half-close": half_close,
    "headers-only": headers_only,
    "garbage-prefix": garbage_prefix,
    "silent": silent,
    "hangup": hangup,
    "wrong-length": wrong_length,
    "endless": endless,
    "slow-headers": slow_headers,
}


def serve_connection(conn, mode):
    with conn:
        if mode == "hangup":
            return
        if read_request(conn) is None:
            return
        HANDLERS[mode](conn)


def main():
    parser = argparse.ArgumentParser(description="a deliberately badly behaved http server")
    parser.add_argument("--mode", choices=MODES, required=True)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--duration", type=float, default=30.0)
    arguments = parser.parse_args()

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((arguments.host, arguments.port))
    listener.listen(128)

    print(listener.getsockname()[1], flush=True)

    deadline = time.monotonic() + arguments.duration
    listener.settimeout(0.5)

    while time.monotonic() < deadline:
        try:
            conn, _ = listener.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        worker = threading.Thread(target=serve_connection, args=(conn, arguments.mode))
        worker.daemon = True
        worker.start()

    listener.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
