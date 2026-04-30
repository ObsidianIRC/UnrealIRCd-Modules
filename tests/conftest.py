import secrets
import string
import subprocess
import tempfile
import time
from collections.abc import Generator
from pathlib import Path

import pytest
from testcontainers.core.container import DockerContainer

ROOT = Path(__file__).parent.parent
BASE_IMAGE = "unrealircd-modules-test-base"
TEST_IMAGE = "unrealircd-modules-test"
TEMPLATE_SRC = ROOT / "docker" / "unrealircd.conf.template"


def _wait_irc(host: str, port: int, timeout: float = 90.0) -> None:
    """Wait until the IRC server responds with 001, handling the PING challenge."""
    from tests.helpers import IRCClient
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with IRCClient(host, port, timeout=5.0) as c:
                c.connect_user("p" + secrets.token_hex(4))
            return
        except (OSError, TimeoutError, ConnectionError):
            time.sleep(1)
    raise TimeoutError(f"IRC {host}:{port} never sent 001 after {timeout}s")


def _cloak_key() -> str:
    chars = string.ascii_letters + string.digits
    return "".join(secrets.choice(chars) for _ in range(80))


def _base_container() -> DockerContainer:
    return (
        DockerContainer(TEST_IMAGE)
        .with_exposed_ports(6667)
        .with_env("IRC_PORT", "6667")
        .with_env("SSL_PORT", "6697")
        .with_env("SERVER_NAME", "irc.test.local")
        .with_env("NETWORK_NAME", "TestNet")
        .with_env("ADMIN_EMAIL", "test@example.com")
        .with_env("CLOAK_KEY1", _cloak_key())
        .with_env("CLOAK_KEY2", _cloak_key())
        .with_env("CLOAK_KEY3", _cloak_key())
    )


@pytest.fixture(scope="session")
def built_images() -> None:
    subprocess.run(
        ["docker", "build", "-t", BASE_IMAGE, "-f", "docker/Dockerfile", "."],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )

    base = TEMPLATE_SRC.read_text()
    test_tmpl = base + "\nexcept ban { mask *; type { connect-flood; maxperip; }; };\n"

    # Bake the modified template into a test image so no bind mount is needed.
    # Bind mounts break when the Docker daemon is on a different host than the
    # process writing to /tmp (e.g. act, remote Docker, Colima).
    with tempfile.TemporaryDirectory() as ctx:
        ctx_path = Path(ctx)
        (ctx_path / "unrealircd.conf.template").write_text(test_tmpl)
        (ctx_path / "Dockerfile").write_text(
            f"FROM {BASE_IMAGE}\n"
            "COPY unrealircd.conf.template /etc/unrealircd/unrealircd.conf.template\n"
        )
        subprocess.run(
            ["docker", "build", "-t", TEST_IMAGE, "."],
            cwd=ctx,
            check=True,
            capture_output=True,
        )


@pytest.fixture(scope="session")
def irc_server(built_images: None) -> Generator[tuple[str, int], None, None]:
    with _base_container() as container:
        host = container.get_container_host_ip()
        port = int(container.get_exposed_port(6667))
        _wait_irc(host, port)
        yield host, port
