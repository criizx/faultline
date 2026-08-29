import socket
import sys
import time


payload = (sys.argv[1] if len(sys.argv) > 1 else "hello through faultline").encode()
started = time.monotonic()
with socket.create_connection(("127.0.0.1", 8080), timeout=5) as connection:
    connection.sendall(payload)
    response = connection.recv(len(payload))
elapsed = (time.monotonic() - started) * 1000
print(f"response={response.decode()} round_trip_ms={elapsed:.1f}")
