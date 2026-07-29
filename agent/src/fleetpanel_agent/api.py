"""FastAPI application.

Every handler is a pure reader over the sampler's last completed document, so no
request path performs measurement. ``/api/v1/health`` and ``/api/v1/schema`` are
unauthenticated on purpose: the first must work for a load balancer or a
``systemd`` health probe, the second contains no host data at all.

There is no write endpoint, no command execution and no shell. That is a design
constraint of the project, not an omission.
"""

from __future__ import annotations

import asyncio
import json
import time
from collections.abc import AsyncIterator
from queue import Empty, Queue
from typing import Any

from fastapi import Depends, FastAPI, HTTPException, Query, Request, Response, status
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, StreamingResponse

from . import AGENT_VERSION, TELEMETRY_SCHEMA
from .config import AgentConfig
from .logging_setup import get_logger
from .sampler import TelemetrySource
from .schema import telemetry_schema
from .security import Authenticator, auth_dependency

_log = get_logger("api")

MAX_HISTORY_SECONDS = 86_400
SSE_KEEPALIVE_SECONDS = 15.0
_POLL_SECONDS = 1.0


def create_app(config: AgentConfig, sampler: TelemetrySource) -> FastAPI:
    guard = auth_dependency(config)
    app = FastAPI(
        title="FleetPanel agent",
        version=AGENT_VERSION,
        summary="Read-only host telemetry in the fleetpanel.telemetry.v1 format",
        description=(
            "Serves the same document over REST that the agent publishes over MQTT.\n\n"
            "This API is read-only by design: it exposes no command execution, no "
            "shell, no shutdown/reboot and no file access."
        ),
        docs_url="/docs" if config.http.enable_docs else None,
        redoc_url=None,
        openapi_url="/openapi.json" if config.http.enable_docs else None,
    )

    if config.http.cors_origins:
        app.add_middleware(
            CORSMiddleware,
            allow_origins=list(config.http.cors_origins),
            allow_credentials=False,
            allow_methods=["GET"],
            allow_headers=["Authorization"],
        )

    app.state.config = config
    app.state.sampler = sampler
    # Kept for introspection and tests; the routes use `guard`, not this.
    app.state.authenticator = Authenticator(config)

    protected = [Depends(guard)]

    # ------------------------------------------------------------------ public

    @app.get("/api/v1/health", tags=["meta"], summary="Liveness probe")
    def health() -> dict[str, Any]:
        latest = sampler.latest()
        return {
            "status": "ok" if latest is not None else "starting",
            "schema": TELEMETRY_SCHEMA,
            "agent_version": AGENT_VERSION,
        }

    @app.get("/api/v1/schema", tags=["meta"], summary="Telemetry JSON Schema")
    def schema() -> dict[str, Any]:
        return telemetry_schema()

    # --------------------------------------------------------------- protected

    @app.get("/api/v1/info", tags=["meta"], dependencies=protected,
             summary="Agent metadata (same document as the retained MQTT meta topic)")
    def info() -> dict[str, Any]:
        meta = sampler.meta()
        meta["history_capacity"] = sampler.history_capacity()
        meta["latest_age_seconds"] = sampler.latest_age_seconds()
        return meta

    @app.get("/api/v1/telemetry", tags=["telemetry"], dependencies=protected,
             summary="Most recent completed sample")
    def telemetry() -> Response:
        latest = sampler.latest()
        if latest is None:
            raise HTTPException(
                status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
                detail="no sample collected yet",
            )
        # Bypass FastAPI's response model machinery: the document is already a plain
        # dict of JSON-safe values and re-validating it every request wastes CPU on a Pi.
        return JSONResponse(content=latest)

    @app.get("/api/v1/history", tags=["telemetry"], dependencies=protected,
             summary="Bounded in-memory sample history")
    def history(
        seconds: int = Query(
            default=300, ge=1, le=MAX_HISTORY_SECONDS,
            description="How far back to return samples, in seconds.",
        ),
    ) -> Response:
        samples = sampler.history(float(seconds))
        return JSONResponse(
            content={
                "schema": TELEMETRY_SCHEMA,
                "requested_seconds": seconds,
                "capacity": sampler.history_capacity(),
                "count": len(samples),
                "samples": samples,
            }
        )

    @app.get("/api/v1/config/public", tags=["meta"], dependencies=protected,
             summary="Non-secret configuration")
    def config_public() -> dict[str, Any]:
        return config.public_view()

    @app.get("/api/v1/stream", tags=["telemetry"], dependencies=protected,
             summary="Server-sent events, one frame per sample")
    async def stream(request: Request) -> StreamingResponse:
        queue = sampler.subscribe()

        async def events() -> AsyncIterator[bytes]:
            try:
                latest = sampler.latest()
                if latest is not None:
                    yield _sse(latest)
                last_keepalive = time.monotonic()
                while True:
                    if await request.is_disconnected():
                        return
                    # Short blocking waits rather than one long one: a disconnected
                    # client is noticed within a second and no worker thread is
                    # parked for the whole keepalive period.
                    try:
                        sample = await asyncio.to_thread(_get, queue, _POLL_SECONDS)
                    except Empty:
                        if time.monotonic() - last_keepalive >= SSE_KEEPALIVE_SECONDS:
                            last_keepalive = time.monotonic()
                            yield b": keepalive\n\n"
                        continue
                    last_keepalive = time.monotonic()
                    yield _sse(sample)
            finally:
                sampler.unsubscribe(queue)

        return StreamingResponse(
            events(),
            media_type="text/event-stream",
            headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
        )

    return app


def _get(queue: Queue[dict[str, Any]], timeout: float) -> dict[str, Any]:
    return queue.get(timeout=timeout)


def _sse(sample: dict[str, Any]) -> bytes:
    return f"event: telemetry\ndata: {json.dumps(sample, separators=(',', ':'))}\n\n".encode()
