import uuid

import pytest

from tests.helpers import IRCClient


def _nick() -> str:
    return "t" + uuid.uuid4().hex[:8]


def _account() -> tuple[str, str, str]:
    name = "acc" + uuid.uuid4().hex[:8]
    return name, f"{name}@example.com", "Passw0rd!"


@pytest.fixture
def client(irc_server: tuple[str, int]) -> IRCClient:
    host, port = irc_server
    with IRCClient(host, port) as c:
        yield c  # type: ignore[misc]


def test_server_connects_and_sends_welcome(irc_server: tuple[str, int]) -> None:
    host, port = irc_server
    with IRCClient(host, port) as c:
        welcome = c.connect_user(_nick())
    assert "001" in welcome


def test_cap_ls_advertises_sasl(irc_server: tuple[str, int]) -> None:
    host, port = irc_server
    with IRCClient(host, port) as c:
        c.send("CAP LS 302")
        line = c.wait_for("CAP", " LS ")
    assert "sasl" in line


def test_cap_ls_advertises_account_registration(irc_server: tuple[str, int]) -> None:
    host, port = irc_server
    with IRCClient(host, port) as c:
        c.send("CAP LS 302")
        line = c.wait_for("CAP", " LS ")
    assert "draft/account-registration" in line


def test_register_new_account(client: IRCClient) -> None:
    client.connect_user(_nick())
    name, email, password = _account()
    line = client.register(name, email, password)
    assert "REGISTER SUCCESS" in line
    assert name in line


def test_register_duplicate_account(client: IRCClient) -> None:
    client.connect_user(_nick())
    name, email, password = _account()
    client.register(name, email, password)
    line = client.register(name, email, password)
    assert "FAIL REGISTER ACCOUNT_EXISTS" in line


def test_identify_correct_password(irc_server: tuple[str, int]) -> None:
    host, port = irc_server
    name, email, password = _account()

    with IRCClient(host, port) as c:
        c.connect_user(_nick())
        c.register(name, email, password)

    with IRCClient(host, port) as c:
        c.connect_user(_nick())
        line = c.identify(name, password)
    assert "IDENTIFY SUCCESS" in line


def test_identify_wrong_password(irc_server: tuple[str, int]) -> None:
    host, port = irc_server
    name, email, password = _account()

    with IRCClient(host, port) as c:
        c.connect_user(_nick())
        c.register(name, email, password)

    with IRCClient(host, port) as c:
        c.connect_user(_nick())
        line = c.identify(name, "wrongpassword!")
    assert "FAIL IDENTIFY" in line


def test_logout(irc_server: tuple[str, int]) -> None:
    host, port = irc_server
    name, email, password = _account()

    with IRCClient(host, port) as c:
        c.connect_user(_nick())
        c.register(name, email, password)

    with IRCClient(host, port) as c:
        c.connect_user(_nick())
        c.identify(name, password)
        line = c.logout()
    assert "LOGOUT SUCCESS" in line


def test_sasl_plain_success(irc_server: tuple[str, int]) -> None:
    host, port = irc_server
    name, email, password = _account()

    with IRCClient(host, port) as c:
        c.connect_user(_nick())
        c.register(name, email, password)

    with IRCClient(host, port) as c:
        line = c.sasl_plain(_nick(), name, password)
    assert "903" in line


def test_sasl_plain_wrong_password(irc_server: tuple[str, int]) -> None:
    host, port = irc_server
    name, email, password = _account()

    with IRCClient(host, port) as c:
        c.connect_user(_nick())
        c.register(name, email, password)

    with IRCClient(host, port) as c:
        line = c.sasl_plain(_nick(), name, "badpassword!")
    assert "904" in line
