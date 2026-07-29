"""API authentication and network scoping.

Two independent gates, both optional and both off by default because the design
target is a trusted LAN:

* **Trusted networks** - when configured, the client address must fall inside one of
  the CIDRs. This is a coarse filter, not a substitute for a token; it exists so an
  agent exposed on a multi-VLAN host does not answer the guest network.
* **Token** - ``bearer`` requires ``Authorization: Bearer <token>``. ``query``
  additionally accepts ``?token=`` for embedded clients whose HTTP stack cannot set
  headers cheaply; it is not the default because query strings land in proxy logs.

Comparisons use :func:`secrets.compare_digest`, so a wrong token takes the same time
as a right one.

The API deliberately exposes no way to run commands, reboot, or reach a shell. There
is no endpoint that writes to the host.
"""

from __future__ import annotations

import ipaddress
import secrets
from collections.abc import Sequence
from typing import Callable

from fastapi import HTTPException, Request, status

from .config import AgentConfig


class AuthError(HTTPException):
    def __init__(self, detail: str, code: int = status.HTTP_401_UNAUTHORIZED) -> None:
        super().__init__(status_code=code, detail=detail, headers={"WWW-Authenticate": "Bearer"})


def parse_networks(entries: Sequence[str]) -> list[ipaddress.IPv4Network | ipaddress.IPv6Network]:
    return [ipaddress.ip_network(item, strict=False) for item in entries]


def address_allowed(
    client_ip: str | None,
    networks: Sequence[ipaddress.IPv4Network | ipaddress.IPv6Network],
) -> bool:
    """Empty ``networks`` means 'no restriction'."""
    if not networks:
        return True
    if not client_ip:
        return False
    try:
        address = ipaddress.ip_address(client_ip)
    except ValueError:
        return False
    return any(address in network for network in networks)


def extract_token(
    auth_header: str | None, query_token: str | None, allow_query: bool
) -> str | None:
    if auth_header:
        scheme, _, value = auth_header.partition(" ")
        if scheme.lower() == "bearer" and value.strip():
            return value.strip()
    if allow_query and query_token:
        return query_token.strip()
    return None


def token_matches(expected: str, presented: str | None) -> bool:
    if not presented:
        return False
    return secrets.compare_digest(expected.encode("utf-8"), presented.encode("utf-8"))


def auth_dependency(config: AgentConfig) -> Callable[[Request], None]:
    """Build the FastAPI dependency that guards the protected routes.

    This returns a plain function rather than the :class:`Authenticator` instance
    itself, and that is load-bearing. FastAPI resolves a dependency's annotations
    against the callable's ``__globals__``; a class instance has no ``__globals__``,
    so with ``from __future__ import annotations`` in effect the annotation is the
    string ``"Request"`` and cannot be resolved. FastAPI then falls back to treating
    the parameter as an ordinary query parameter, and every protected route answers
    ``422 {"loc": ["query", "request"]}`` instead of running.

    Found on Debian 11 with Python 3.9. A nested function carries the module's
    globals, so the annotation resolves on every supported interpreter.
    """
    authenticator = Authenticator(config)

    def require_auth(request: Request) -> None:
        authenticator.check(request)

    return require_auth


class Authenticator:
    """Authentication policy. Not used directly as a dependency - see
    :func:`auth_dependency` for why."""

    def __init__(self, config: AgentConfig) -> None:
        self.mode = config.http.auth_mode
        self.token = config.http.api_token
        self.allow_query = config.http.auth_mode == "query"
        self.networks = parse_networks(config.http.trusted_networks)

    def check(self, request: Request) -> None:
        client_ip = request.client.host if request.client else None
        if not address_allowed(client_ip, self.networks):
            raise AuthError(
                "client address is outside the configured trusted networks",
                code=status.HTTP_403_FORBIDDEN,
            )
        if self.mode == "none":
            return
        presented = extract_token(
            request.headers.get("authorization"),
            request.query_params.get("token"),
            self.allow_query,
        )
        if not token_matches(self.token, presented):
            raise AuthError("missing or invalid API token")

    def __call__(self, request: Request) -> None:
        self.check(request)
