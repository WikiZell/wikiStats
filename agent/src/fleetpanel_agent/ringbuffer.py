"""Bounded in-memory sample history.

Deliberately not a database. The buffer is sized from the configured retention
window and sampling interval and hard-capped, so memory use is constant regardless of
uptime - a requirement on a Raspberry Pi that may run for a year.
"""

from __future__ import annotations

import threading
from collections import deque
from collections.abc import Iterable
from typing import Any

# 8 hours at the 2 s default is 14 400 samples; each sample is a few hundred bytes of
# Python objects, so this ceiling keeps worst-case history well under ~30 MB.
MAX_SAMPLES = 20_000


def capacity_for(history_seconds: int, sample_interval: float) -> int:
    """Number of slots needed to cover ``history_seconds``, clamped to ``MAX_SAMPLES``."""
    if history_seconds <= 0 or sample_interval <= 0:
        return 0
    needed = int(history_seconds / sample_interval) + 1
    return max(1, min(needed, MAX_SAMPLES))


class HistoryBuffer:
    """Thread-safe fixed-capacity ring of telemetry documents."""

    def __init__(self, capacity: int) -> None:
        self._capacity = max(0, capacity)
        self._items: deque[tuple[float, dict[str, Any]]] = deque(maxlen=self._capacity or 1)
        self._lock = threading.Lock()
        self._enabled = self._capacity > 0

    @property
    def capacity(self) -> int:
        return self._capacity

    def __len__(self) -> int:
        with self._lock:
            return len(self._items) if self._enabled else 0

    def append(self, monotonic_ts: float, sample: dict[str, Any]) -> None:
        if not self._enabled:
            return
        with self._lock:
            self._items.append((monotonic_ts, sample))

    def since(self, now: float, seconds: float) -> list[dict[str, Any]]:
        """Samples no older than ``seconds`` relative to the monotonic clock."""
        if not self._enabled:
            return []
        cutoff = now - max(seconds, 0.0)
        with self._lock:
            return [sample for ts, sample in self._items if ts >= cutoff]

    def all(self) -> list[dict[str, Any]]:
        if not self._enabled:
            return []
        with self._lock:
            return [sample for _, sample in self._items]

    def clear(self) -> None:
        with self._lock:
            self._items.clear()

    def extend(self, entries: Iterable[tuple[float, dict[str, Any]]]) -> None:
        for ts, sample in entries:
            self.append(ts, sample)
