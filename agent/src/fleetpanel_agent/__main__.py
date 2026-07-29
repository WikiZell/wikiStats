"""Command-line entry point: ``fleetpanel-agent`` / ``python -m fleetpanel_agent``."""

from __future__ import annotations

import argparse
import sys

from . import AGENT_VERSION
from .config import DEFAULT_CONFIG_PATH, ConfigError, load_config
from .logging_setup import configure_logging, get_logger
from .service import AgentService


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="fleetpanel-agent",
        description="FleetPanel / WikiStats telemetry agent (read-only host metrics)",
    )
    parser.add_argument(
        "-c", "--config", default=str(DEFAULT_CONFIG_PATH),
        help=f"path to config.toml (default: {DEFAULT_CONFIG_PATH})",
    )
    parser.add_argument("--port", type=int, default=None, help="override http.port")
    parser.add_argument("--host", default=None, help="override http.host")
    parser.add_argument(
        "--check", action="store_true",
        help="validate the configuration, print the resolved identity, and exit",
    )
    parser.add_argument("--version", action="version", version=f"fleetpanel-agent {AGENT_VERSION}")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        config = load_config(args.config)
    except ConfigError as exc:
        configure_logging("INFO", "text")
        get_logger("config").error("%s", exc)
        return 2

    if args.port is not None:
        config.http.port = args.port
    if args.host is not None:
        config.http.host = args.host
    try:
        config.validate_cross_section()
    except ConfigError as exc:
        configure_logging("INFO", "text")
        get_logger("config").error("%s", exc)
        return 2

    configure_logging(config.logging.level, config.logging.format)
    service = AgentService(config)

    if args.check:
        log = get_logger("check")
        log.info("configuration OK", extra={"fields": service.describe()})
        return 0

    service.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
