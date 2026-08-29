import socketserver


class EchoHandler(socketserver.BaseRequestHandler):
    def handle(self):
        while data := self.request.recv(65536):
            self.request.sendall(data)


with socketserver.ThreadingTCPServer(("0.0.0.0", 9000), EchoHandler) as server:
    server.serve_forever()
