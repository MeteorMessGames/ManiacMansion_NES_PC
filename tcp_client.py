import socket, json, sys

def send(cmd, **kw):
    s = socket.create_connection(("127.0.0.1", 4370), timeout=5)
    msg = json.dumps({"cmd": cmd, **kw}) + "\n"
    s.sendall(msg.encode())
    data = b""
    while not data.endswith(b"\n"):
        chunk = s.recv(65536)
        if not chunk: break
        data += chunk
    s.close()
    return json.loads(data.decode())

if __name__ == "__main__":
    cmd = sys.argv[1]
    kw = {}
    for a in sys.argv[2:]:
        k, v = a.split("=", 1)
        try: v = int(v, 0)
        except ValueError: pass
        kw[k] = v
    print(json.dumps(send(cmd, **kw)))
