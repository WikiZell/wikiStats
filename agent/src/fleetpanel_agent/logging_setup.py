"""Structured logging with built-in flood control.

Sensor failures are expected on odd hardware and must never fill the journal. Every
collector reports failures through :func:`log_throttled`, which emits the first
occurrence immediately and then at most one line per ``interval`` seconds per key,
including a count of how many were suppressed.
"""

from __future__ import annotations

import json
import logging
import sys
import time
from typing import Any

_LOGGER_NAME = "fleetpanel"

# key -> (last_emit_monotonic, suppressed_count)
_throttle_state: dict[str, tuple[float, int]] = {}

_DEFAULT_THROTTLE_SECONDS = 300.0


class JsonFormatter(logging.Formatter):
    """Emit one JSON object per line, suitable for journald/Loki ingestion."""

    def format(self, record: logging.LogRecord) -> str:
        payload: dict[str, Any] = {
            "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(record.created)),
            "level": record.levelname,
            "logger": record.name,
            "message": record.getMessage(),
        }
        extra = getattr(record, "fields", None)
        if isinstance(extra, dict):
            payload.update(extra)
        if record.exc_info:
            payload["exception"] = self.formatException(record.exc_info)
        return json.dumps(payload, default=str)


class TextFormatter(logging.Formatter):
    """Human readable single-line format, no timestamp (journald adds one)."""

    def __init__(self) -> None:
        super().__init__(fmt="%(levelname)-7s %(name)s: %(message)s")

    def format(self, record: logging.LogRecord) -> str:
        base = super().format(record)
        extra = getattr(record, "fields", None)
        if isinstance(extra, dict) and extra:
            joined = " ".join(f"{k}={v}" for k, v in extra.items())
            return f"{base} [{joined}]"
        return base


def configure_logging(level: str = "INFO", fmt: str = "text") -> None:
    """Install the root handler. Safe to call more than once."""
    root = logging.getLogger()
    for handler in list(root.handlers):
        root.removeHandler(handler)
    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(JsonFormatter() if fmt == "json" else TextFormatter())
    root.addHandler(handler)
    root.setLevel(getattr(logging, level.upper(), logging.INFO))
    # uvicorn installs its own noisy access logger; keep it but at WARNING.
    logging.getLogger("uvicorn.access").setLevel(logging.WARNING)


def get_logger(name: str | None = None) -> logging.Logger:
    return logging.getLogger(_LOGGER_NAME if name is None else f"{_LOGGER_NAME}.{name}")


def log_throttled(
    logger: logging.Logger,
    level: int,
    key: str,
    message: str,
    *,
    interval: float = _DEFAULT_THROTTLE_SECONDS,
    exc_info: bool = False,
    **fields: Any,
) -> None:
    """Log ``message`` at most once per ``interval`` seconds for the given ``key``."""
    now = time.monotonic()
    last, suppressed = _throttle_state.get(key, (0.0, 0))
    if last and now - last < interval:
        _throttle_state[key] = (last, suppressed + 1)
        return
    if suppressed:
        fields = {**fields, "suppressed": suppressed}
    _throttle_state[key] = (now, 0)
    logger.log(level, message, exc_info=exc_info, extra={"fields": fields})


def reset_throttle() -> None:
    """Clear throttle bookkeeping. Used by tests."""
    _throttle_state.clear()
