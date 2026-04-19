import base64
import socket
import time


class IRCClient:
    def __init__(self, host: str, port: int, timeout: float = 15.0) -> None:
        self._host = host
        self._port = port
        self._timeout = timeout
        self._sock: socket.socket | None = None
        self._buf = ""

    def connect(self) -> None:
        self._sock = socket.create_connection((self._host, self._port), timeout=self._timeout)
        self._sock.settimeout(self._timeout)

    def disconnect(self) -> None:
        if self._sock:
            try:
                self._sock.sendall(b"QUIT :bye\r\n")
                self._sock.shutdown(socket.SHUT_WR)
                # drain until server closes so it processes the QUIT cleanly
                self._sock.settimeout(3.0)
                while self._sock.recv(4096):
                    pass
            except OSError:
                pass
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def __enter__(self) -> "IRCClient":
        self.connect()
        return self

    def __exit__(self, *_: object) -> None:
        self.disconnect()

    def send(self, line: str) -> None:
        assert self._sock is not None
        self._sock.sendall((line + "\r\n").encode())

    def readline(self) -> str:
        assert self._sock is not None
        while "\n" not in self._buf:
            chunk = self._sock.recv(4096).decode(errors="replace")
            if not chunk:
                raise ConnectionError(f"Connection closed by server. Last buffer: {self._buf!r}")
            self._buf += chunk
        line, self._buf = self._buf.split("\n", 1)
        line = line.rstrip("\r")
        if line.upper().startswith("PING "):
            self.send("PONG " + line[5:])
            return self.readline()
        return line

    def wait_for(self, *tokens: str, timeout: float = 20.0, any_of: bool = False) -> str:
        assert self._sock is not None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._sock.settimeout(max(0.5, deadline - time.monotonic()))
            try:
                line = self.readline()
            except socket.timeout:
                continue
            match = any(t in line for t in tokens) if any_of else all(t in line for t in tokens)
            if match:
                return line
        raise TimeoutError(f"Timed out waiting for {tokens!r}")

    def connect_user(self, nick: str) -> str:
        self.send(f"NICK {nick}")
        self.send(f"USER {nick} 0 * :{nick}")
        return self.wait_for(" 001 ")

    def sasl_plain(self, nick: str, username: str, password: str) -> str:
        self.send("CAP LS 302")
        self.wait_for("CAP", " LS ")
        self.send("CAP REQ :sasl")
        self.wait_for("CAP", "ACK")
        self.send(f"NICK {nick}")
        self.send(f"USER {nick} 0 * :{nick}")
        self.send("AUTHENTICATE PLAIN")
        self.wait_for("AUTHENTICATE +")
        payload = base64.b64encode(f"\x00{username}\x00{password}".encode()).decode()
        self.send(f"AUTHENTICATE {payload}")
        result = self.wait_for("903", "904", any_of=True)
        self.send("CAP END")
        if "903" in result:
            self.wait_for(" 001 ")
        return result

    def register(self, name: str, email: str, password: str) -> str:
        self.send(f"REGISTER {name} {email} {password}")
        return self.wait_for("REGISTER")

    def identify(self, account: str, password: str) -> str:
        self.send(f"IDENTIFY {account} {password}")
        return self.wait_for("IDENTIFY")

    def logout(self) -> str:
        self.send("LOGOUT")
        return self.wait_for("LOGOUT")
