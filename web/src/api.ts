// Typed client for the panel's REST API.
//
// Two rules the rest of the app relies on:
//   * every mutating call carries the CSRF token from the current session, and
//   * a 401 anywhere resets the session so the shell falls back to the login view
//     instead of leaving the user staring at a page that silently stopped saving.

export interface Session {
  authenticated: boolean;
  password_set: boolean;
  provisioning: boolean;
  web_enabled: boolean;
  csrf?: string;
}

export interface WifiStatus {
  phase: string;
  ssid: string;
  ip: string;
  rssi: number;
  ap_ssid: string;
  ap_ip: string;
  hostname: string;
  saved_networks: number;
  message: string;
}

export interface TransportStatus {
  mode: string;
  active: string;
  mqtt_connected: boolean;
  mqtt_messages: number;
  http_polls: number;
  http_failures: number;
  mdns_running: boolean;
  pending_discoveries: number;
}

export interface Status {
  product: string;
  firmware: string;
  telemetry_schema: string;
  uptime_ms: number;
  free_heap: number;
  min_free_heap: number;
  heap_size: number;
  sketch_size: number;
  free_sketch_space: number;
  chip_model: string;
  cpu_mhz: number;
  reset_reason: string;
  boot_count: number;
  config_status: string;
  password_set: boolean;
  fs_total: number;
  fs_used: number;
  wifi: WifiStatus;
  transport: TransportStatus;
  device_count: number;
  log_console_clients: number;
}

export interface DeviceMetrics {
  cpu_percent?: number;
  cpu_temperature_c?: number;
  memory_percent?: number;
  memory_total_bytes?: number;
  memory_used_bytes?: number;
  storage_percent?: number;
  storage_total_bytes?: number;
  storage_free_bytes?: number;
  rx_bytes_per_second?: number;
  tx_bytes_per_second?: number;
  uptime_seconds?: number;
  os_name?: string;
  hardware_model?: string;
  agent_version?: string;
}

export interface Device {
  id: string;
  name: string;
  alias: string;
  label: string;
  base_url: string;
  path: string;
  platform: string;
  source: string;
  auth: string;
  /** Write-only. The API reports `token_set` and never returns the value itself. */
  token?: string;
  token_set: boolean;
  enabled: boolean;
  hidden: boolean;
  carousel: boolean;
  order: number;
  ever_received: boolean;
  age_seconds: number;
  failures: number;
  last_error: string;
  last_transport: string;
  state: "online" | "stale" | "offline" | "waiting";
  metrics?: DeviceMetrics;
}

export interface Discovered {
  id: string;
  name: string;
  base_url: string;
  platform: string;
  source: string;
  auth: string;
}

export interface DeviceList {
  devices: Device[];
  discovered: Discovered[];
}

export interface ScanNetwork {
  ssid: string;
  rssi: number;
  channel: number;
  secure: boolean;
  known: boolean;
}

export interface PanelConfig {
  schema: string;
  version: number;
  panel: {
    name: string;
    timezone_offset_minutes: number;
    units: "binary" | "decimal";
    temperature_unit: "c" | "f";
  };
  wifi: { hostname: string; networks: { ssid: string; password: string; priority: number; hidden: boolean }[] };
  mqtt: {
    enabled: boolean;
    host: string;
    port: number;
    username: string;
    password: string;
    tls: boolean;
    base_topic: string;
    client_id: string;
  };
  transport: {
    mode: "http" | "mqtt" | "auto";
    poll_interval_ms: number;
    http_timeout_ms: number;
    max_payload_bytes: number;
    mqtt_fresh_ms: number;
  };
  discovery: { mdns_enabled: boolean; auto_add: boolean; interval_s: number };
  display: { brightness: number; screen_timeout_s: number; dim_brightness: number; rotation: number };
  carousel: {
    enabled: boolean;
    interval_s: number;
    idle_resume_s: number;
    wrap: boolean;
    include_offline: boolean;
  };
  thresholds: {
    cpu_warn: number;
    cpu_crit: number;
    cpu_temp_warn: number;
    cpu_temp_crit: number;
    ram_warn: number;
    ram_crit: number;
    disk_warn: number;
    disk_crit: number;
    stale_s: number;
    offline_s: number;
  };
  web: { enabled: boolean; username: string; password_set: boolean; session_timeout_s: number };
  logging: { level: string; telnet_enabled: boolean; telnet_port: number };
  ota: { arduino_ota_enabled: boolean; password: string };
  devices: unknown[];
}

export class ApiError extends Error {
  constructor(
    message: string,
    readonly status: number,
  ) {
    super(message);
  }
}

let csrf = "";
const listeners = new Set<(session: Session) => void>();

export function onSessionChange(fn: (session: Session) => void): void {
  listeners.add(fn);
}

function announce(session: Session): void {
  csrf = session.csrf ?? "";
  for (const fn of listeners) fn(session);
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const method = (init.method ?? "GET").toUpperCase();
  const headers = new Headers(init.headers);
  if (init.body !== undefined) headers.set("Content-Type", "application/json");
  if (method !== "GET" && method !== "HEAD") headers.set("X-CSRF-Token", csrf);

  const response = await fetch(path, { ...init, headers, credentials: "same-origin" });
  if (response.status === 401) {
    announce({ authenticated: false, password_set: true, provisioning: false, web_enabled: true });
    throw new ApiError("Session expired. Sign in again.", 401);
  }
  const text = await response.text();
  const body = text ? safeParse(text) : {};
  if (!response.ok) {
    const message =
      typeof body === "object" && body !== null && "error" in body
        ? String((body as { error: unknown }).error)
        : `HTTP ${response.status}`;
    throw new ApiError(message, response.status);
  }
  return body as T;
}

function safeParse(text: string): unknown {
  try {
    return JSON.parse(text);
  } catch {
    return text;
  }
}

export const api = {
  async session(): Promise<Session> {
    const session = await request<Session>("/api/session");
    announce(session);
    return session;
  },
  async login(password: string): Promise<Session> {
    const session = await request<Session>("/api/login", {
      method: "POST",
      body: JSON.stringify({ password }),
    });
    announce({ ...session, password_set: true, provisioning: false, web_enabled: true });
    return session;
  },
  async logout(): Promise<void> {
    await request("/api/logout", { method: "POST" });
    announce({ authenticated: false, password_set: true, provisioning: false, web_enabled: true });
  },
  setPassword(password: string, currentPassword?: string): Promise<{ ok: boolean }> {
    return request("/api/password", {
      method: "POST",
      body: JSON.stringify({ password, current_password: currentPassword ?? "" }),
    });
  },
  status: () => request<Status>("/api/status"),
  config: () => request<PanelConfig>("/api/config"),
  saveConfig: (patch: Partial<PanelConfig>) =>
    request<{ ok: boolean }>("/api/config", { method: "PUT", body: JSON.stringify(patch) }),
  devices: () => request<DeviceList>("/api/devices"),
  addDevice: (device: Partial<Device>) =>
    request<{ ok: boolean; id: string }>("/api/devices", {
      method: "POST",
      body: JSON.stringify(device),
    }),
  updateDevice: (id: string, patch: Partial<Device>) =>
    request<{ ok: boolean }>(`/api/devices/${encodeURIComponent(id)}`, {
      method: "PUT",
      body: JSON.stringify(patch),
    }),
  deleteDevice: (id: string) =>
    request<{ ok: boolean }>(`/api/devices/${encodeURIComponent(id)}`, { method: "DELETE" }),
  approveDiscovered: (id: string) =>
    request<{ ok: boolean }>(`/api/discovered/${encodeURIComponent(id)}/approve`, {
      method: "POST",
    }),
  reorderDevices: (order: string[]) =>
    request<{ ok: boolean }>("/api/devices/order", {
      method: "POST",
      body: JSON.stringify({ order }),
    }),
  discoveryScan: () => request<{ ok: boolean }>("/api/discovery/scan", { method: "POST" }),
  wifiScan: () => request<{ scanning: boolean; networks: ScanNetwork[] }>("/api/wifi/scan"),
  wifiConnect: (ssid: string, password: string, hidden = false) =>
    request<{ ok: boolean; message: string }>("/api/wifi/connect", {
      method: "POST",
      body: JSON.stringify({ ssid, password, hidden }),
    }),
  wifiForget: (ssid: string) =>
    request<{ ok: boolean }>("/api/wifi/forget", { method: "POST", body: JSON.stringify({ ssid }) }),
  wifiForgetAll: () => request<{ ok: boolean }>("/api/wifi/forget-all", { method: "POST" }),
  restart: () => request<{ ok: boolean }>("/api/restart", { method: "POST" }),
  factoryReset: () =>
    request<{ ok: boolean }>("/api/factory-reset", {
      method: "POST",
      body: JSON.stringify({ confirm: true }),
    }),
  async logs(): Promise<string> {
    const response = await fetch("/api/logs", { credentials: "same-origin" });
    if (!response.ok) throw new ApiError(`HTTP ${response.status}`, response.status);
    return response.text();
  },
  backupUrl: (withSecrets: boolean) => (withSecrets ? "/api/backup?secrets=1" : "/api/backup"),
  restore: (json: string) => request<{ ok: boolean }>("/api/restore", { method: "POST", body: json }),
  async uploadFirmware(file: File, onProgress: (percent: number) => void): Promise<void> {
    // XHR rather than fetch: upload progress events are the whole point here, and
    // flashing an ESP32 over Wi-Fi takes long enough that a progress bar matters.
    await new Promise<void>((resolve, reject) => {
      const form = new FormData();
      form.append("firmware", file, file.name);
      const xhr = new XMLHttpRequest();
      xhr.open("POST", "/api/firmware");
      xhr.setRequestHeader("X-CSRF-Token", csrf);
      xhr.upload.addEventListener("progress", (event) => {
        if (event.lengthComputable) onProgress(Math.round((event.loaded / event.total) * 100));
      });
      xhr.addEventListener("load", () =>
        xhr.status >= 200 && xhr.status < 300
          ? resolve()
          : reject(new ApiError(xhr.responseText || `HTTP ${xhr.status}`, xhr.status)),
      );
      xhr.addEventListener("error", () => reject(new ApiError("upload failed", 0)));
      xhr.send(form);
    });
  },
};
