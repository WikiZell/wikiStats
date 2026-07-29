# Security

## Threat model, stated plainly

WikiStats is designed for a **trusted LAN**. It is not hardened against an attacker
who is already on your network and actively targeting it, and this page says so
rather than implying otherwise.

What it does guarantee:

* The agent **cannot** be made to run anything. There is no command endpoint, no
  shell, no reboot, no file access. Every route is a `GET`, and a test enumerates
  the route table to prove it.
* Credentials are not logged, not returned by any read endpoint, and not included
  in a backup unless you explicitly ask.
* The panel's web interface requires an administrator password before it will
  change anything.

## Agent

### Authentication

Three modes, `none` by default because the design target is a LAN:

| Mode | Behaviour |
| ---- | --------- |
| `none` | no authentication (default) |
| `bearer` | requires `Authorization: Bearer <token>` |
| `query` | additionally accepts `?token=…` for embedded clients whose HTTP stack cannot set headers cheaply |

`query` is not the default because query strings land in proxy and access logs.

Tokens are compared with `secrets.compare_digest`, so a wrong token takes the same
time as a right one. Minimum length 16 characters.

`/api/v1/health` and `/api/v1/schema` stay unauthenticated even in token mode: the
first must work for a health probe, and the second contains no host data at all.

### Trusted networks

```toml
[http]
trusted_networks = ["192.168.0.0/16", "10.0.0.0/8"]
```

A coarse filter, not a substitute for a token. It exists so an agent on a
multi-VLAN host does not answer the guest network. Empty means no restriction.

### What is not exposed

* `GET /api/v1/config/public` reports `api_token_set: true`, never the token.
  MQTT credentials become `username_set` / `password_set` booleans.
* The raw `/etc/machine-id` is never published — only a salted SHA-256 truncated to
  12 hex characters.
* MQTT passwords appear in the log as `password=<set>` or `password=<unset>`.

### Configuration file

`/etc/fleetpanel-agent/config.toml` is mode `0640`, owned `root:fleetpanel`. It may
contain an API token and an MQTT password, so it is group-readable only.

### systemd hardening

The unit is locked down about as far as the collectors allow:

```ini
NoNewPrivileges=yes       ProtectSystem=strict      ProtectHome=yes
PrivateTmp=yes            PrivateDevices=yes        ProtectKernelTunables=yes
ProtectKernelModules=yes  ProtectKernelLogs=yes     ProtectControlGroups=yes
RestrictNamespaces=yes    RestrictRealtime=yes      RestrictSUIDSGID=yes
LockPersonality=yes       MemoryDenyWriteExecute=yes
SystemCallFilter=@system-service                    CapabilityBoundingSet=
MemoryMax=256M            TasksMax=64
```

Two exceptions are load-bearing and commented in the unit file:

* `RestrictAddressFamilies` includes `AF_NETLINK` — psutil needs it to enumerate
  network interfaces.
* `ProtectProc=default`, not `invisible` — otherwise `process_count` only ever sees
  the agent's own process.

The agent binds a port above 1024 and therefore needs no capabilities at all.

## Panel

### Administrator password

* PBKDF2-HMAC-SHA256, 10 000 iterations, 32-byte output, random 16-byte salt from
  the hardware RNG. Roughly 150 ms on an ESP32 with hardware SHA — slow enough to
  make offline guessing expensive, fast enough not to be a denial of service.
* Only the hash and salt are stored. No plaintext, anywhere, ever.
* Changing it requires the current password *even with a valid session*, and
  invalidates every session.

### Sessions and CSRF

* Cookie `wsid`, 64 hex characters from `esp_random()`, `HttpOnly`, `SameSite=Strict`.
* A **separate** CSRF token, required in `X-CSRF-Token` on every mutating request.
  A cookie alone is not enough because a browser attaches it to cross-site requests
  too; the CSRF token is only readable by same-origin script.
* Server-side expiry with a sliding window, so an admin mid-edit is not logged out.
* Four concurrent sessions maximum.

### Login rate limiting

Per client address. Five failures start a lockout that doubles — 5 s, 10 s, 20 s …
capped at five minutes per attempt. Successful login resets the counter.

### Forced password setup

Until a password is set, the web interface refuses every mutating request with
`403 set an administrator password before changing settings`.

**The one exception**, and the reasoning: on first boot the panel is an open access
point with no network and no password. There is no way to bootstrap without
allowing *something*. While the setup portal is up and no password exists, the Wi-Fi
provisioning endpoints (`/api/wifi/*`) accept requests unauthenticated. Physical
proximity to an open access point is the trust boundary at that moment. Once the
panel joins a network the portal comes down and the exception ends.

### Secrets in the API

`GET /api/config` replaces every Wi-Fi password, the MQTT password, per-device API
tokens, the admin hash and salt, and the OTA password with the sentinel
`__redacted__`. Values that are simply unset come back as `""`, so the UI can
distinguish "configured but hidden" from "not configured".

`PUT /api/config` ignores `__redacted__`, so a client can send back a document it
received redacted without destroying credentials it never saw. There is a native
test for exactly this round trip.

### Backup

* Plain backup: secrets redacted. Safe to store or email.
* `?secrets=1`: contains Wi-Fi passwords, the MQTT password, API tokens and the
  admin password hash **in clear text**. It is a credential; treat it as one. It
  requires an authenticated session and is logged as a warning.

Restoring a plain backup keeps the passwords already on the panel rather than
clearing them — otherwise a restore would lock you out of your own broker.

### OTA

ArduinoOTA **refuses to start without a password**. An unauthenticated OTA listener
on a LAN is a remote code execution primitive, so the default is off rather than
open. Web upload is always available behind the admin session and CSRF check.

Both paths write the inactive OTA slot and only switch over after a successful
verify.

### Remote log console

TCP port 23, enabled by default, **output only** — anything received is read and
discarded so it can never become a command channel. It carries hostnames, IP
addresses and device names, which is why it can be switched off on the Security
page. It is unauthenticated: treat it as you would a serial port on the desk.

### Disabling the web interface

Security → *Keep this configuration website enabled*. With it off, only the
touchscreen configures the panel. Re-enable it from the panel or with a factory
reset.

## Known limitations

These are real, and listed rather than glossed over:

1. **The agent API is unencrypted by default.** Telemetry crosses the LAN in clear
   text. HTTPS is not offered because certificate management on an ESP32 with no
   clock is worse than the problem it solves. Put a reverse proxy in front if you
   need it.
2. **The panel's web interface is HTTP.** The admin password is sent in clear text
   over your LAN. Same reasoning.
3. **MQTT TLS does not authenticate the broker.** `tls = true` on the panel uses
   `setInsecure()`: it buys transport confidentiality, not server identity. The
   panel has no trust store and no clock at boot. The *agent* side does verify,
   and supports `tls_ca_file`.
4. **The setup portal is an open access point.** Anyone in radio range during first
   boot can reach the Wi-Fi endpoints. The window is small and requires physical
   proximity, but it is real.
5. **Anyone with broker access can read all telemetry.** Use per-device ACLs
   restricted to `fleetpanel/v1/devices/<id>/#` on a shared broker.
6. **No audit log.** The panel logs configuration changes to its ring buffer, which
   is 6 KiB and does not survive a reboot.
7. **`trusted_networks` trusts the source address**, which is spoofable on a network
   where you have already lost.

## Reporting a vulnerability

Open a GitHub issue for anything already public. For anything that is not, contact
the maintainers privately first.
