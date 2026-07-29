"""RAM and swap.

``used_bytes`` follows psutil's definition (total minus free minus buffers/cache on
Linux), and ``usage_percent`` is derived from ``available_bytes`` so it matches what
``free -m`` and every Linux dashboard reports - a machine with 6 GiB in page cache is
not 90% full.
"""

from __future__ import annotations

import logging
from typing import Any

import psutil

from ..logging_setup import get_logger, log_throttled

_log = get_logger("memory")

_NULL_MEMORY: dict[str, Any] = {
    "total_bytes": None,
    "available_bytes": None,
    "used_bytes": None,
    "free_bytes": None,
    "usage_percent": None,
    "swap_total_bytes": None,
    "swap_used_bytes": None,
    "swap_free_bytes": None,
    "swap_usage_percent": None,
}


class MemoryCollector:
    def collect(self) -> dict[str, Any]:
        result = dict(_NULL_MEMORY)
        try:
            vm = psutil.virtual_memory()
        except Exception:  # noqa: BLE001
            log_throttled(_log, logging.WARNING, "mem.virtual", "virtual_memory failed")
            return result

        total = int(vm.total)
        available = int(getattr(vm, "available", 0))
        used = int(getattr(vm, "used", max(total - available, 0)))
        free = int(getattr(vm, "free", max(total - used, 0)))
        result.update(
            {
                "total_bytes": total,
                "available_bytes": available,
                "used_bytes": used,
                "free_bytes": free,
                "usage_percent": (
                    round((total - available) / total * 100.0, 1) if total > 0 else None
                ),
            }
        )

        try:
            sw = psutil.swap_memory()
        except Exception:  # noqa: BLE001 - swapless systems are common on Pi images
            log_throttled(_log, logging.DEBUG, "mem.swap", "swap_memory unavailable")
            return result

        swap_total = int(sw.total)
        swap_used = int(sw.used)
        swap_free = int(sw.free)
        result.update(
            {
                "swap_total_bytes": swap_total,
                "swap_used_bytes": swap_used,
                "swap_free_bytes": swap_free,
                "swap_usage_percent": (
                    round(swap_used / swap_total * 100.0, 1) if swap_total > 0 else 0.0
                ),
            }
        )
        return result
