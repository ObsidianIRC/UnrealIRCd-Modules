import uuid
from collections.abc import Generator
from contextlib import contextmanager
from pathlib import Path

import pytest
from testcontainers.core.container import DockerContainer

from tests.conftest import _base_container, _wait_irc
from tests.helpers import IRCClient

MINIMAL_MODULE = """\
#include "unrealircd.h"
ModuleHeader MOD_HEADER = {{"third/{name}", "1.0", "Test module", "test", 1}};
MOD_INIT() {{ return MOD_SUCCESS; }}
MOD_LOAD() {{ return MOD_SUCCESS; }}
MOD_UNLOAD() {{ return MOD_SUCCESS; }}
"""


@contextmanager
def _server_with_modules(
    custom_modules_path: Path,
) -> Generator[tuple[DockerContainer, str, int], None, None]:
    container = _base_container().with_volume_mapping(
        str(custom_modules_path), "/home/unrealircd/unrealircd/custom-modules", "ro"
    )
    with container:
        host = container.get_container_host_ip()
        port = int(container.get_exposed_port(6667))
        _wait_irc(host, port)
        yield container, host, port


@pytest.fixture(scope="module")
def valid_module_server(
    built_images: None, tmp_path_factory: pytest.TempPathFactory
) -> Generator[tuple[DockerContainer, str, int, str], None, None]:
    mod_dir = tmp_path_factory.mktemp("custom_modules")
    mod_name = "hello_" + uuid.uuid4().hex[:6]
    (mod_dir / f"{mod_name}.c").write_text(MINIMAL_MODULE.format(name=mod_name))

    with _server_with_modules(mod_dir) as (container, host, port):
        yield container, host, port, mod_name


def test_custom_module_so_exists(
    valid_module_server: tuple[DockerContainer, str, int, str],
) -> None:
    container, _, _, mod_name = valid_module_server
    result = container.exec(f"ls /home/unrealircd/unrealircd/modules/third/{mod_name}.so")
    assert result.exit_code == 0, f"Expected {mod_name}.so in modules/third/"


def test_custom_module_compile_logged(
    valid_module_server: tuple[DockerContainer, str, int, str],
) -> None:
    container, _, _, mod_name = valid_module_server
    stdout, stderr = container.get_logs()
    logs = (stdout + stderr).decode(errors="replace")
    assert f"Compiled: {mod_name}.so" in logs


def test_builtin_modules_still_present(
    valid_module_server: tuple[DockerContainer, str, int, str],
) -> None:
    container, _, _, _ = valid_module_server
    for so in ("obsidianirc.so", "o-filehost.so"):
        result = container.exec(f"ls /home/unrealircd/unrealircd/modules/third/{so}")
        assert result.exit_code == 0, f"Built-in {so} missing after custom module load"


def test_server_accepts_connections_with_custom_module(
    valid_module_server: tuple[DockerContainer, str, int, str],
) -> None:
    _, host, port, _ = valid_module_server
    with IRCClient(host, port) as c:
        welcome = c.connect_user("t" + uuid.uuid4().hex[:8])
    assert "001" in welcome


def test_bad_module_does_not_crash_server(built_images: None, tmp_path: Path) -> None:
    bad_dir = tmp_path / "bad_modules"
    bad_dir.mkdir()
    (bad_dir / "bad_module.c").write_text("this is not valid C code !!!!")

    with _server_with_modules(bad_dir) as (container, host, port):
        stdout, stderr = container.get_logs()
        logs = (stdout + stderr).decode(errors="replace")
        assert "WARNING: Failed to compile bad_module" in logs

        with IRCClient(host, port) as c:
            welcome = c.connect_user("t" + uuid.uuid4().hex[:8])
        assert "001" in welcome
