// The panel's configuration website and REST API.
//
// Served by ESPAsyncWebServer, which runs its own task. Handlers therefore do three
// things and nothing else: take the AppState lock, read or mutate, release. No
// handler calls LVGL, blocks on the network, or writes flash directly - a
// configuration change sets the dirty flag and the network task performs the
// (slow) write on its own schedule.
//
// Security model, documented in full in docs/security.md:
//   * Administrator password, hashed with PBKDF2-HMAC-SHA256 (10 000 iterations,
//     random 16-byte salt). No plaintext password is ever stored or returned.
//   * Session cookie, HttpOnly + SameSite=Strict, with a server-side expiry.
//   * A separate CSRF token, required in X-CSRF-Token on every mutating request.
//   * Login attempts rate limited per client address with escalating lockout.
//   * GET /api/config redacts every secret.
//   * The whole web interface can be switched off after setup.
//
// First-boot exception: while the setup portal is up (no station connection yet),
// the Wi-Fi provisioning endpoints work without a password. Physical proximity to
// an open access point is the trust boundary at that point; there is no way to
// bootstrap otherwise. Everything else stays locked until a password is set.
#pragma once

#include <cstdint>
#include <string>

namespace net {

void startWebServer();
void stopWebServer();
bool webServerRunning();

// Set (or replace) the administrator password. Hashes with a fresh salt.
bool setAdminPassword(const std::string& password, std::string& errorOut);
bool verifyAdminPassword(const std::string& password);

// True when a reboot has been requested by an API call; main() performs it once the
// response has been flushed.
bool restartPending();
bool factoryResetPending();
void clearPendingActions();

}  // namespace net
