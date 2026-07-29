"""Filesystem usage with pseudo-filesystem and duplicate-mount filtering.

Naive aggregation over ``psutil.disk_partitions()`` triple-counts on any modern
desktop: snaps are squashfs loop mounts, containers add overlays, and bind mounts
report the same block device twice. The aggregate here counts each backing device at
most once and skips filesystems that are not real storage.

Selection rules, in order:

1. If ``include`` is non-empty, only those mountpoints (and their subtrees) are
   considered - it is an explicit allow-list and it disables the pseudo filter.
2. Otherwise drop pseudo filesystem types and well-known synthetic mount prefixes.
3. Drop anything matching ``exclude``.
4. Drop filesystems below ``min_total_bytes`` (EFI partitions, tiny boot volumes)
   unless the mountpoint is ``/``.
5. Deduplicate by backing device, preferring the shortest mountpoint.
"""

from __future__ import annotations

import logging
from collections.abc import Callable, Iterable, Sequence
from typing import Any, NamedTuple

import psutil

from ..logging_setup import get_logger, log_throttled

_log = get_logger("storage")

PSEUDO_FSTYPES: frozenset[str] = frozenset(
    {
        "autofs",
        "binfmt_misc",
        "bpf",
        "cgroup",
        "cgroup2",
        "configfs",
        "debugfs",
        "devpts",
        "devtmpfs",
        "efivarfs",
        "fusectl",
        "hugetlbfs",
        "mqueue",
        "nsfs",
        "overlay",
        "proc",
        "pstore",
        "ramfs",
        "rpc_pipefs",
        "securityfs",
        "selinuxfs",
        "squashfs",
        "sysfs",
        "tmpfs",
        "tracefs",
    }
)

PSEUDO_MOUNT_PREFIXES: tuple[str, ...] = (
    "/proc",
    "/sys",
    "/dev",
    "/run",
    "/snap",
    "/var/snap",
    "/var/lib/docker",
    "/var/lib/containers",
    "/var/lib/kubelet",
)


class Partition(NamedTuple):
    """The subset of ``psutil._common.sdiskpart`` this module uses."""

    device: str
    mountpoint: str
    fstype: str


class Usage(NamedTuple):
    total: int
    used: int
    free: int
    percent: float


UsageFn = Callable[[str], Usage]


def _under(path: str, prefix: str) -> bool:
    """``path`` is ``prefix`` or lives under it.

    ``"/"`` is special-cased to mean the root filesystem itself. Treating it as a
    prefix would make ``include = ["/"]`` match every mount on the machine, which is
    the opposite of what an operator writing an allow-list intends.
    """
    prefix = prefix.rstrip("/")
    if not prefix:
        return path == "/"
    return path == prefix or path.startswith(prefix + "/")


def _is_pseudo(part: Partition) -> bool:
    fstype = (part.fstype or "").lower()
    if fstype in PSEUDO_FSTYPES or fstype.startswith("fuse."):
        return True
    return any(_under(part.mountpoint, prefix) for prefix in PSEUDO_MOUNT_PREFIXES)


def select_partitions(
    partitions: Iterable[Partition],
    *,
    include: Sequence[str] = (),
    exclude: Sequence[str] = (),
    include_pseudo: bool = False,
) -> list[Partition]:
    """Apply the include/exclude/pseudo rules. Pure function; unit tested directly."""
    chosen: list[Partition] = []
    for part in partitions:
        if include:
            if not any(_under(part.mountpoint, item) for item in include):
                continue
        elif not include_pseudo and _is_pseudo(part):
            continue
        if any(_under(part.mountpoint, item) for item in exclude):
            continue
        chosen.append(part)
    # Shortest mountpoint first so "/" beats "/mnt/bind-of-root" during dedup.
    chosen.sort(key=lambda p: (len(p.mountpoint), p.mountpoint))
    return chosen


def _dedup_key(part: Partition) -> str:
    device = (part.device or "").strip()
    # Real block devices identify the backing store; anything else (network shares,
    # synthetic sources) is only unique by mountpoint.
    if device.startswith("/dev/"):
        return f"dev:{device}"
    if device and ":" in device:  # nfs/cifs: host:/export
        return f"net:{device}"
    return f"mp:{part.mountpoint}"


def build_mounts(
    partitions: Iterable[Partition],
    usage_fn: UsageFn,
    *,
    include: Sequence[str] = (),
    exclude: Sequence[str] = (),
    include_pseudo: bool = False,
    min_total_bytes: int = 64 * 1024 * 1024,
) -> list[dict[str, Any]]:
    """Return the deduplicated per-mount list, newest rules applied."""
    mounts: list[dict[str, Any]] = []
    seen: set[str] = set()
    for part in select_partitions(
        partitions, include=include, exclude=exclude, include_pseudo=include_pseudo
    ):
        key = _dedup_key(part)
        if key in seen:
            continue
        try:
            usage = usage_fn(part.mountpoint)
        except (OSError, PermissionError):
            # Unreadable mount (disconnected network share, restricted namespace).
            log_throttled(
                _log,
                logging.DEBUG,
                f"storage.usage.{part.mountpoint}",
                "cannot stat filesystem",
                mountpoint=part.mountpoint,
            )
            continue
        total = int(usage.total)
        if total <= 0:
            continue
        if total < min_total_bytes and part.mountpoint != "/":
            continue
        seen.add(key)
        used = int(usage.used)
        free = int(usage.free)
        mounts.append(
            {
                "device": part.device or None,
                "mountpoint": part.mountpoint,
                "filesystem": part.fstype or None,
                "total_bytes": total,
                "used_bytes": used,
                "free_bytes": free,
                "usage_percent": round(float(usage.percent), 1),
            }
        )
    return mounts


def aggregate(mounts: Sequence[dict[str, Any]]) -> dict[str, Any]:
    """Fleet-level totals. Each backing device has already been counted once."""
    if not mounts:
        return {
            "total_bytes": None,
            "used_bytes": None,
            "free_bytes": None,
            "usage_percent": None,
            "mounts": None,
        }
    total = sum(int(m["total_bytes"]) for m in mounts)
    used = sum(int(m["used_bytes"]) for m in mounts)
    free = sum(int(m["free_bytes"]) for m in mounts)
    return {
        "total_bytes": total,
        "used_bytes": used,
        "free_bytes": free,
        "usage_percent": round(used / total * 100.0, 1) if total > 0 else None,
        "mounts": list(mounts),
    }


def _psutil_usage(mountpoint: str) -> Usage:
    raw = psutil.disk_usage(mountpoint)
    return Usage(int(raw.total), int(raw.used), int(raw.free), float(raw.percent))


class StorageCollector:
    def __init__(
        self,
        *,
        include: Sequence[str] = (),
        exclude: Sequence[str] = (),
        include_pseudo: bool = False,
        min_total_bytes: int = 64 * 1024 * 1024,
    ) -> None:
        self.include = tuple(include)
        self.exclude = tuple(exclude)
        self.include_pseudo = include_pseudo
        self.min_total_bytes = min_total_bytes

    def _partitions(self) -> list[Partition]:
        try:
            raw = psutil.disk_partitions(all=self.include_pseudo or bool(self.include))
        except Exception:  # noqa: BLE001
            log_throttled(_log, logging.WARNING, "storage.parts", "disk_partitions failed")
            return []
        return [Partition(p.device, p.mountpoint, p.fstype) for p in raw]

    def collect(self) -> dict[str, Any]:
        mounts = build_mounts(
            self._partitions(),
            _psutil_usage,
            include=self.include,
            exclude=self.exclude,
            include_pseudo=self.include_pseudo,
            min_total_bytes=self.min_total_bytes,
        )
        return aggregate(mounts)
