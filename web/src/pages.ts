// The configuration pages.
//
// Every page follows the same shape: fetch what it needs, build DOM, and save
// through a partial PUT /api/config. Partial patches matter - the panel merges them
// into the live configuration and ignores redacted secrets, so a page that only
// knows about thresholds cannot accidentally erase the MQTT password.

import { api, type Device, type DeviceList, type PanelConfig, type Status } from "./api";
import { append, button, card, checkbox, clear, confirmAction, field, h, numberInput, select, textInput, toast } from "./dom";
import { age, bytes, celsius, duration, percent, rate, signal } from "./format";

export interface PageContext {
  host: HTMLElement;
  refreshShell: () => Promise<void>;
}

type PageRenderer = (context: PageContext) => Promise<void>;

export interface PageDefinition {
  id: string;
  title: string;
  hint: string;
  render: PageRenderer;
  badge?: (status: Status) => number;
}

function heading(context: PageContext, page: PageDefinition): void {
  clear(context.host);
  append(context.host, [h("h1", {}, page.title), h("p", { class: "page-hint" }, page.hint)]);
}

function meter(value: number | undefined, warn: number, crit: number): HTMLElement {
  const clamped = Math.max(0, Math.min(100, value ?? 0));
  const level = value === undefined ? "" : value >= crit ? "crit" : value >= warn ? "warn" : "";
  return h(
    "div",
    { class: "meter" },
    h("span", { class: level, style: `width:${clamped}%` }),
  );
}

async function saveConfig(patch: Partial<PanelConfig>, what: string): Promise<void> {
  try {
    await api.saveConfig(patch);
    toast(`${what} saved`);
  } catch (error) {
    toast(error instanceof Error ? error.message : String(error), "error");
  }
}

// ============================================================== dashboard

const dashboard: PageDefinition = {
  id: "dashboard",
  title: "Dashboard",
  hint: "Live view of every monitored machine, exactly as the panel sees it.",
  async render(context) {
    heading(context, dashboard);
    const [status, list] = await Promise.all([api.status(), api.devices()]);

    const summary = card(
      "Panel",
      null,
      h(
        "dl",
        { class: "kv" },
        h("dt", {}, "Firmware"),
        h("dd", {}, `${status.product} ${status.firmware}`),
        h("dt", {}, "Wi-Fi"),
        h("dd", {}, `${status.wifi.phase} · ${status.wifi.ssid || "-"} · ${status.wifi.ip || "-"}`),
        h("dt", {}, "Transport"),
        h(
          "dd",
          {},
          `${status.transport.active} (mode ${status.transport.mode})` +
            (status.transport.mqtt_connected ? " · MQTT connected" : ""),
        ),
        h("dt", {}, "Uptime"),
        h("dd", {}, duration(Math.floor(status.uptime_ms / 1000))),
        h("dt", {}, "Free heap"),
        h("dd", {}, bytes(status.free_heap)),
      ),
    );

    const cards = list.devices
      .filter((device) => !device.hidden && device.enabled)
      .map((device) => deviceCard(device));

    append(context.host, [
      summary,
      list.discovered.length > 0
        ? h(
            "div",
            { class: "banner" },
            `${list.discovered.length} device(s) discovered and waiting for approval. `,
            h("a", { href: "#discovered" }, "Review them"),
          )
        : null,
      cards.length > 0
        ? h("div", { class: "grid" }, ...cards)
        : card("No devices yet", null, h("p", {}, "Add one on the Devices page, or run a discovery scan.")),
    ]);
  },
};

function deviceCard(device: Device): HTMLElement {
  const metrics = device.metrics ?? {};
  return h(
    "section",
    { class: "card" },
    h(
      "div",
      { class: "row", style: "justify-content:space-between" },
      h("h2", { style: "margin:0" }, device.label || device.id),
      h("span", { class: `pill ${device.state}` }, device.state),
    ),
    h(
      "dl",
      { class: "kv", style: "margin-top:12px" },
      h("dt", {}, "CPU"),
      h("dd", {}, `${percent(metrics.cpu_percent)}  ${celsius(metrics.cpu_temperature_c)}`),
      h("dt", {}, "Memory"),
      h(
        "dd",
        {},
        `${percent(metrics.memory_percent)} · ${bytes(metrics.memory_used_bytes)} / ${bytes(metrics.memory_total_bytes)}`,
      ),
      h("dt", {}, "Storage"),
      h(
        "dd",
        {},
        `${percent(metrics.storage_percent)} · ${bytes(metrics.storage_free_bytes)} free of ${bytes(metrics.storage_total_bytes)}`,
      ),
      h("dt", {}, "Network"),
      h("dd", {}, `↓ ${rate(metrics.rx_bytes_per_second)}   ↑ ${rate(metrics.tx_bytes_per_second)}`),
      h("dt", {}, "Uptime"),
      h("dd", {}, duration(metrics.uptime_seconds)),
      h("dt", {}, "Updated"),
      h("dd", {}, device.ever_received ? age(device.age_seconds) : "never"),
    ),
    device.last_error ? h("p", { class: "hint", style: "margin:10px 0 0" }, device.last_error) : null,
  );
}

// ================================================================ devices

const devices: PageDefinition = {
  id: "devices",
  title: "Devices",
  hint: "Add machines by address, rename them, reorder the carousel, or remove them.",
  async render(context) {
    heading(context, devices);
    const list = await api.devices();

    const url = textInput("", { placeholder: "http://192.168.1.50:8770" });
    const name = textInput("", { placeholder: "optional display name" });
    const auth = select("none", [
      { value: "none", label: "No authentication" },
      { value: "bearer", label: "Bearer token" },
      { value: "query", label: "Query token (?token=)" },
    ]);
    const token = h("input", { type: "password", placeholder: "API token", autocomplete: "off" });

    const addCard = card(
      "Add a device manually",
      "Use this when the agent is on another subnet, or when mDNS is blocked.",
      field("Base URL", url, "Scheme, host and port of the agent. The telemetry path is added automatically."),
      field("Name", name),
      field("Authentication", auth),
      field("Token", token, "Only needed when the agent's auth_mode is bearer or query."),
      button(
        "Add device",
        async () => {
          if (!url.value.trim()) {
            toast("A base URL is required", "error");
            return;
          }
          try {
            await api.addDevice({
              base_url: url.value.trim(),
              name: name.value.trim(),
              auth: auth.value,
              token: token.value,
            });
            toast("Device added");
            await devices.render(context);
          } catch (error) {
            toast(error instanceof Error ? error.message : String(error), "error");
          }
        },
        "primary",
      ),
    );

    append(context.host, [addCard, deviceTable(list, context)]);
  },
};

function deviceTable(list: DeviceList, context: PageContext): HTMLElement {
  if (list.devices.length === 0) {
    return card("Configured devices", null, h("p", {}, "None yet."));
  }
  const rows = list.devices.map((device, index) => {
    const alias = textInput(device.alias, { placeholder: device.name });
    return h(
      "tr",
      {},
      h("td", {}, h("span", { class: `pill ${device.state}` }, device.state)),
      h("td", {}, alias),
      h(
        "td",
        {},
        h("div", {}, device.base_url),
        h("small", { style: "color:var(--text-dim)" }, `${device.source} · ${device.platform} · ${device.id}`),
      ),
      h("td", { class: "num" }, device.ever_received ? age(device.age_seconds) : "never"),
      h(
        "td",
        {},
        h(
          "div",
          { class: "row" },
          button("Save", async () => {
            await api.updateDevice(device.id, { alias: alias.value.trim() });
            toast("Saved");
            await devices.render(context);
          }),
          button(device.enabled ? "Disable" : "Enable", async () => {
            await api.updateDevice(device.id, { enabled: !device.enabled });
            await devices.render(context);
          }),
          button(device.hidden ? "Show" : "Hide", async () => {
            await api.updateDevice(device.id, { hidden: !device.hidden });
            await devices.render(context);
          }),
          button("↑", async () => {
            const order = list.devices.map((d) => d.id);
            if (index > 0) {
              [order[index - 1], order[index]] = [order[index], order[index - 1]];
              await api.reorderDevices(order);
              await devices.render(context);
            }
          }),
          button("↓", async () => {
            const order = list.devices.map((d) => d.id);
            if (index < order.length - 1) {
              [order[index + 1], order[index]] = [order[index], order[index + 1]];
              await api.reorderDevices(order);
              await devices.render(context);
            }
          }),
          button(
            "Delete",
            async () => {
              if (!confirmAction(`Remove ${device.label}? Its history is lost.`)) return;
              await api.deleteDevice(device.id);
              toast("Device removed");
              await devices.render(context);
            },
            "danger",
          ),
        ),
      ),
    );
  });

  return card(
    "Configured devices",
    "Order here is the order the carousel uses.",
    h(
      "div",
      { class: "table-wrap" },
      h(
        "table",
        {},
        h(
          "thead",
          {},
          h(
            "tr",
            {},
            h("th", {}, "State"),
            h("th", {}, "Alias"),
            h("th", {}, "Address"),
            h("th", { class: "num" }, "Updated"),
            h("th", {}, "Actions"),
          ),
        ),
        h("tbody", {}, ...rows),
      ),
    ),
  );
}

// ============================================================= discovered

const discovered: PageDefinition = {
  id: "discovered",
  title: "Discovered devices",
  hint: "Agents found over mDNS or MQTT that have not been added yet.",
  badge: (status) => status.transport.pending_discoveries,
  async render(context) {
    heading(context, discovered);
    const list = await api.devices();

    const scan = card(
      "Scan now",
      "Discovery also runs automatically on the interval set under HTTP and MQTT.",
      button(
        "Scan for agents",
        async () => {
          await api.discoveryScan();
          toast("Scan queued - results appear within a few seconds");
          setTimeout(() => void discovered.render(context), 4000);
        },
        "primary",
      ),
    );

    const rows = list.discovered.map((entry) =>
      h(
        "tr",
        {},
        h("td", {}, entry.name || entry.id),
        h("td", {}, entry.base_url),
        h("td", {}, `${entry.source} · ${entry.platform}`),
        h(
          "td",
          {},
          h(
            "div",
            { class: "row" },
            button(
              "Add",
              async () => {
                await api.approveDiscovered(entry.id);
                toast("Device added");
                await discovered.render(context);
                await context.refreshShell();
              },
              "primary",
            ),
            button(
              "Ignore",
              async () => {
                await api.deleteDevice(entry.id);
                await discovered.render(context);
                await context.refreshShell();
              },
              "danger",
            ),
          ),
        ),
      ),
    );

    append(context.host, [
      scan,
      rows.length > 0
        ? card(
            "Waiting for approval",
            null,
            h(
              "div",
              { class: "table-wrap" },
              h(
                "table",
                {},
                h("thead", {}, h("tr", {}, h("th", {}, "Name"), h("th", {}, "Address"), h("th", {}, "Source"), h("th", {}, ""))),
                h("tbody", {}, ...rows),
              ),
            ),
          )
        : card("Waiting for approval", null, h("p", {}, "Nothing pending.")),
    ]);
  },
};

// ================================================================ display

const display: PageDefinition = {
  id: "display",
  title: "Display",
  hint: "Brightness, screen timeout and orientation of the panel itself.",
  async render(context) {
    heading(context, display);
    const config = await api.config();

    const brightness = h("input", {
      type: "range",
      min: 5,
      max: 100,
      value: String(config.display.brightness),
    });
    const brightnessValue = h("span", {}, `${config.display.brightness}%`);
    brightness.addEventListener("input", () => {
      brightnessValue.textContent = `${brightness.value}%`;
    });

    const dim = numberInput(config.display.dim_brightness, 0, 100);
    const timeout = numberInput(config.display.screen_timeout_s, 0, 3600);
    const rotation = select(String(config.display.rotation), [
      { value: "1", label: "Landscape (USB left)" },
      { value: "3", label: "Landscape inverted (USB right)" },
      { value: "0", label: "Portrait" },
      { value: "2", label: "Portrait inverted" },
    ]);
    const units = select(config.panel.units, [
      { value: "binary", label: "Binary (KiB, MiB, GiB)" },
      { value: "decimal", label: "Decimal (kB, MB, GB)" },
    ]);
    const panelName = textInput(config.panel.name);
    const timezone = numberInput(config.panel.timezone_offset_minutes, -840, 840);

    append(context.host, [
      card(
        "Backlight",
        null,
        field("Brightness", h("div", { class: "row" }, brightness, brightnessValue) as HTMLElement),
        field("Dim level", dim, "Brightness used after the screen timeout. 0 turns the backlight off."),
        field("Screen timeout (seconds)", timeout, "0 keeps the display at full brightness for ever."),
      ),
      card(
        "Panel",
        null,
        field("Panel name", panelName),
        field("Orientation", rotation, "Takes effect after a restart."),
        field("Units", units),
        field("Time zone offset (minutes)", timezone),
      ),
      button(
        "Save display settings",
        () =>
          saveConfig(
            {
              display: {
                brightness: Number(brightness.value),
                dim_brightness: Number(dim.value),
                screen_timeout_s: Number(timeout.value),
                rotation: Number(rotation.value),
              },
              panel: {
                name: panelName.value,
                units: units.value as "binary" | "decimal",
                temperature_unit: config.panel.temperature_unit,
                timezone_offset_minutes: Number(timezone.value),
              },
            },
            "Display settings",
          ),
        "primary",
      ),
    ]);
  },
};

// =============================================================== carousel

const carousel: PageDefinition = {
  id: "carousel",
  title: "Carousel",
  hint: "How the panel rotates through devices when nobody is touching it.",
  async render(context) {
    heading(context, carousel);
    const config = await api.config();

    const enabled = checkbox(config.carousel.enabled, "Rotate automatically");
    const interval = numberInput(config.carousel.interval_s, 3, 120);
    const idle = numberInput(config.carousel.idle_resume_s, 0, 3600);
    const wrap = checkbox(config.carousel.wrap, "Wrap around after the last device");
    const includeOffline = checkbox(config.carousel.include_offline, "Include offline devices");

    append(context.host, [
      card(
        "Rotation",
        null,
        enabled,
        field("Seconds per device", interval, "Between 3 and 120."),
        field("Resume delay (seconds)", idle, "How long after the last touch the rotation restarts."),
        wrap,
        includeOffline,
      ),
      button(
        "Save carousel settings",
        () =>
          saveConfig(
            {
              carousel: {
                enabled: (enabled.querySelector("input") as HTMLInputElement).checked,
                interval_s: Number(interval.value),
                idle_resume_s: Number(idle.value),
                wrap: (wrap.querySelector("input") as HTMLInputElement).checked,
                include_offline: (includeOffline.querySelector("input") as HTMLInputElement).checked,
              },
            },
            "Carousel settings",
          ),
        "primary",
      ),
    ]);
  },
};

// =================================================================== wifi

const wifi: PageDefinition = {
  id: "wifi",
  title: "Wi-Fi",
  hint: "Join a network, keep several saved, or forget them. Saved passwords are never shown again.",
  async render(context) {
    heading(context, wifi);
    const [status, config] = await Promise.all([api.status(), api.config()]);

    const listHost = h("div", {}, h("p", { class: "hint" }, "Scanning…"));
    const refreshScan = async (): Promise<void> => {
      try {
        const result = await api.wifiScan();
        clear(listHost);
        if (result.networks.length === 0) {
          append(listHost, [h("p", { class: "hint" }, result.scanning ? "Scanning…" : "No networks found.")]);
          return;
        }
        const rows = result.networks.map((network) =>
          h(
            "tr",
            {},
            h("td", {}, network.ssid),
            h("td", { class: "num" }, `${network.rssi} dBm (${signal(network.rssi)})`),
            h("td", {}, network.secure ? "secured" : "open"),
            h("td", {}, network.known ? "saved" : ""),
            h(
              "td",
              {},
              button("Join", () => promptJoin(network.ssid, network.secure)),
            ),
          ),
        );
        append(listHost, [
          h(
            "div",
            { class: "table-wrap" },
            h(
              "table",
              {},
              h("thead", {}, h("tr", {}, h("th", {}, "Network"), h("th", { class: "num" }, "Signal"), h("th", {}, "Security"), h("th", {}, ""), h("th", {}, ""))),
              h("tbody", {}, ...rows),
            ),
          ),
        ]);
      } catch (error) {
        toast(error instanceof Error ? error.message : String(error), "error");
      }
    };

    const ssidInput = textInput("", { placeholder: "SSID" });
    const passwordInput = h("input", { type: "password", placeholder: "password", autocomplete: "off" });
    const hiddenBox = checkbox(false, "Hidden network");

    function promptJoin(ssid: string, secure: boolean): void {
      ssidInput.value = ssid;
      passwordInput.value = "";
      if (secure) passwordInput.focus();
      ssidInput.scrollIntoView({ behavior: "smooth", block: "center" });
    }

    const savedRows = config.wifi.networks.map((network) =>
      h(
        "tr",
        {},
        h("td", {}, network.ssid),
        h("td", { class: "num" }, String(network.priority)),
        h("td", {}, network.hidden ? "hidden" : ""),
        h(
          "td",
          {},
          button(
            "Forget",
            async () => {
              if (!confirmAction(`Forget ${network.ssid}?`)) return;
              await api.wifiForget(network.ssid);
              toast("Network forgotten");
              await wifi.render(context);
            },
            "danger",
          ),
        ),
      ),
    );

    append(context.host, [
      card(
        "Current connection",
        null,
        h(
          "dl",
          { class: "kv" },
          h("dt", {}, "State"),
          h("dd", {}, status.wifi.phase),
          h("dt", {}, "Network"),
          h("dd", {}, status.wifi.ssid || "-"),
          h("dt", {}, "Address"),
          h("dd", {}, status.wifi.ip || "-"),
          h("dt", {}, "Signal"),
          h("dd", {}, status.wifi.rssi ? `${status.wifi.rssi} dBm (${signal(status.wifi.rssi)})` : "-"),
          h("dt", {}, "Hostname"),
          h("dd", {}, `${status.wifi.hostname}.local`),
          h("dt", {}, "Message"),
          h("dd", {}, status.wifi.message || "-"),
        ),
      ),
      card(
        "Join a network",
        "The password is sent once and stored on the panel. It is never returned by the API.",
        field("SSID", ssidInput),
        field("Password", passwordInput),
        hiddenBox,
        button(
          "Connect",
          async () => {
            if (!ssidInput.value.trim()) {
              toast("An SSID is required", "error");
              return;
            }
            try {
              const result = await api.wifiConnect(
                ssidInput.value.trim(),
                passwordInput.value,
                (hiddenBox.querySelector("input") as HTMLInputElement).checked,
              );
              passwordInput.value = "";
              toast(result.message || "Connecting…");
            } catch (error) {
              toast(error instanceof Error ? error.message : String(error), "error");
            }
          },
          "primary",
        ),
      ),
      card("Available networks", null, h("div", { class: "row" }, button("Rescan", () => void refreshScan())), listHost),
      card(
        "Saved networks",
        "Tried in priority order, lowest number first.",
        savedRows.length > 0
          ? h(
              "div",
              { class: "table-wrap" },
              h(
                "table",
                {},
                h("thead", {}, h("tr", {}, h("th", {}, "SSID"), h("th", { class: "num" }, "Priority"), h("th", {}, ""), h("th", {}, ""))),
                h("tbody", {}, ...savedRows),
              ),
            )
          : h("p", {}, "None saved."),
        h(
          "div",
          { class: "row", style: "margin-top:12px" },
          button(
            "Forget all networks",
            async () => {
              if (!confirmAction("Forget every saved network? The panel will fall back to its setup portal.")) return;
              await api.wifiForgetAll();
              toast("All networks forgotten");
              await wifi.render(context);
            },
            "danger",
          ),
        ),
      ),
    ]);

    void refreshScan();
  },
};

// ============================================================== transport

const transport: PageDefinition = {
  id: "transport",
  title: "HTTP and MQTT",
  hint: "How the panel collects telemetry, and how often it looks for new agents.",
  async render(context) {
    heading(context, transport);
    const config = await api.config();

    const mode = select(config.transport.mode, [
      { value: "auto", label: "Automatic (MQTT when it is delivering, otherwise HTTP)" },
      { value: "http", label: "HTTP only" },
      { value: "mqtt", label: "MQTT only" },
    ]);
    const poll = numberInput(config.transport.poll_interval_ms, 1000, 300000);
    const timeout = numberInput(config.transport.http_timeout_ms, 500, 30000);
    const maxPayload = numberInput(config.transport.max_payload_bytes, 1024, 65536);

    const mqttEnabled = checkbox(config.mqtt.enabled, "Use an MQTT broker");
    const host = textInput(config.mqtt.host, { placeholder: "broker.lan" });
    const port = numberInput(config.mqtt.port, 1, 65535);
    const username = textInput(config.mqtt.username);
    const password = h("input", { type: "password", placeholder: config.mqtt.password ? "unchanged" : "", autocomplete: "off" });
    const tls = checkbox(config.mqtt.tls, "TLS");
    const baseTopic = textInput(config.mqtt.base_topic);

    const mdns = checkbox(config.discovery.mdns_enabled, "Discover agents over mDNS");
    const autoAdd = checkbox(config.discovery.auto_add, "Add discovered devices automatically");
    const interval = numberInput(config.discovery.interval_s, 10, 3600);

    append(context.host, [
      card(
        "Transport",
        null,
        field("Mode", mode),
        field("Poll interval (ms)", poll),
        field("HTTP timeout (ms)", timeout),
        field("Maximum response size (bytes)", maxPayload, "Responses above this are rejected before parsing."),
      ),
      card(
        "MQTT",
        "Entirely optional. The panel works over plain HTTP with no broker present.",
        mqttEnabled,
        field("Host", host),
        field("Port", port),
        field("Username", username),
        field("Password", password, "Leave empty to keep the stored password."),
        tls,
        field("Base topic", baseTopic, "Must match the agents' mqtt.base_topic."),
      ),
      card("Discovery", null, mdns, autoAdd, field("Scan interval (seconds)", interval)),
      button(
        "Save transport settings",
        () => {
          const patch: Partial<PanelConfig> = {
            transport: {
              mode: mode.value as "http" | "mqtt" | "auto",
              poll_interval_ms: Number(poll.value),
              http_timeout_ms: Number(timeout.value),
              max_payload_bytes: Number(maxPayload.value),
              mqtt_fresh_ms: config.transport.mqtt_fresh_ms,
            },
            mqtt: {
              enabled: (mqttEnabled.querySelector("input") as HTMLInputElement).checked,
              host: host.value.trim(),
              port: Number(port.value),
              username: username.value,
              // An empty field means "unchanged": the sentinel is what the panel's
              // merge logic looks for.
              password: password.value === "" ? "__redacted__" : password.value,
              tls: (tls.querySelector("input") as HTMLInputElement).checked,
              base_topic: baseTopic.value.trim(),
              client_id: config.mqtt.client_id,
            },
            discovery: {
              mdns_enabled: (mdns.querySelector("input") as HTMLInputElement).checked,
              auto_add: (autoAdd.querySelector("input") as HTMLInputElement).checked,
              interval_s: Number(interval.value),
            },
          };
          return saveConfig(patch, "Transport settings");
        },
        "primary",
      ),
    ]);
  },
};

// ============================================================= thresholds

const thresholds: PageDefinition = {
  id: "thresholds",
  title: "Thresholds",
  hint: "When a value turns amber or red on the panel, and when a device counts as stale or offline.",
  async render(context) {
    heading(context, thresholds);
    const config = await api.config();
    const t = config.thresholds;

    const inputs = {
      cpu_warn: numberInput(t.cpu_warn, 1, 100),
      cpu_crit: numberInput(t.cpu_crit, 1, 100),
      cpu_temp_warn: numberInput(t.cpu_temp_warn, 20, 140),
      cpu_temp_crit: numberInput(t.cpu_temp_crit, 20, 140),
      ram_warn: numberInput(t.ram_warn, 1, 100),
      ram_crit: numberInput(t.ram_crit, 1, 100),
      disk_warn: numberInput(t.disk_warn, 1, 100),
      disk_crit: numberInput(t.disk_crit, 1, 100),
      stale_s: numberInput(t.stale_s, 1, 3600),
      offline_s: numberInput(t.offline_s, 2, 86400),
    };

    append(context.host, [
      card(
        "Usage",
        "Percentages. The critical value is raised automatically if it is set below the warning value.",
        h(
          "div",
          { class: "grid" },
          field("CPU warning", inputs.cpu_warn),
          field("CPU critical", inputs.cpu_crit),
          field("RAM warning", inputs.ram_warn),
          field("RAM critical", inputs.ram_crit),
          field("Disk warning", inputs.disk_warn),
          field("Disk critical", inputs.disk_crit),
        ),
      ),
      card(
        "Temperature",
        "Degrees Celsius.",
        h("div", { class: "grid" }, field("CPU temp warning", inputs.cpu_temp_warn), field("CPU temp critical", inputs.cpu_temp_crit)),
      ),
      card(
        "Freshness",
        "Measured from the last sample the panel received, not from the agent's own status.",
        h("div", { class: "grid" }, field("Stale after (seconds)", inputs.stale_s), field("Offline after (seconds)", inputs.offline_s)),
      ),
      button(
        "Save thresholds",
        () =>
          saveConfig(
            {
              thresholds: Object.fromEntries(
                Object.entries(inputs).map(([key, input]) => [key, Number(input.value)]),
              ) as PanelConfig["thresholds"],
            },
            "Thresholds",
          ),
        "primary",
      ),
    ]);
  },
};

// =============================================================== security

const security: PageDefinition = {
  id: "security",
  title: "Security",
  hint: "Administrator password, session lifetime, and whether this website stays reachable.",
  async render(context) {
    heading(context, security);
    const [config, status] = await Promise.all([api.config(), api.status()]);

    const current = h("input", { type: "password", autocomplete: "current-password" });
    const next = h("input", { type: "password", autocomplete: "new-password" });
    const confirm = h("input", { type: "password", autocomplete: "new-password" });
    const timeoutInput = numberInput(config.web.session_timeout_s, 60, 86400);
    const webEnabled = checkbox(config.web.enabled, "Keep this configuration website enabled");
    const telnet = checkbox(config.logging.telnet_enabled, "Wi-Fi log console (read-only)");
    const telnetPort = numberInput(config.logging.telnet_port, 1, 65535);
    const logLevel = select(config.logging.level, [
      { value: "error", label: "error" },
      { value: "warn", label: "warn" },
      { value: "info", label: "info" },
      { value: "debug", label: "debug" },
      { value: "trace", label: "trace" },
    ]);
    const otaEnabled = checkbox(config.ota.arduino_ota_enabled, "Allow OTA uploads from PlatformIO");

    append(context.host, [
      !status.password_set
        ? h("div", { class: "banner" }, "No administrator password is set. Until one is, the panel refuses every settings change from this website.")
        : null,
      card(
        "Administrator password",
        "Hashed with PBKDF2-HMAC-SHA256 and a random salt. Changing it signs out every session.",
        status.password_set ? field("Current password", current) : null,
        field("New password", next, "At least 8 characters."),
        field("Repeat new password", confirm),
        button(
          "Set password",
          async () => {
            if (next.value !== confirm.value) {
              toast("The two new passwords do not match", "error");
              return;
            }
            try {
              await api.setPassword(next.value, current.value);
              toast("Password changed - sign in again");
              await context.refreshShell();
            } catch (error) {
              toast(error instanceof Error ? error.message : String(error), "error");
            }
          },
          "primary",
        ),
      ),
      card(
        "Sessions and access",
        null,
        field("Session timeout (seconds)", timeoutInput),
        webEnabled,
        h(
          "p",
          { class: "hint" },
          "Disabling the website leaves only the touchscreen. Re-enable it with a factory reset or from the panel itself.",
        ),
      ),
      card(
        "Diagnostics access",
        "The Wi-Fi log console streams the same lines as the USB serial port. It accepts no input.",
        telnet,
        field("Console port", telnetPort),
        field("Log level", logLevel),
        otaEnabled,
      ),
      button(
        "Save security settings",
        () =>
          saveConfig(
            {
              web: {
                enabled: (webEnabled.querySelector("input") as HTMLInputElement).checked,
                username: config.web.username,
                password_set: config.web.password_set,
                session_timeout_s: Number(timeoutInput.value),
              },
              logging: {
                level: logLevel.value,
                telnet_enabled: (telnet.querySelector("input") as HTMLInputElement).checked,
                telnet_port: Number(telnetPort.value),
              },
              ota: {
                arduino_ota_enabled: (otaEnabled.querySelector("input") as HTMLInputElement).checked,
                password: "__redacted__",
              },
            },
            "Security settings",
          ),
        "primary",
      ),
    ]);
  },
};

// ================================================================= system

const system: PageDefinition = {
  id: "system",
  title: "System information",
  hint: "Firmware, memory, filesystem and the panel's own log.",
  async render(context) {
    heading(context, system);
    const status = await api.status();

    const logBox = h("pre", { class: "log" }, "Loading…");
    const loadLog = async (): Promise<void> => {
      try {
        logBox.textContent = (await api.logs()) || "(empty)";
        logBox.scrollTop = logBox.scrollHeight;
      } catch (error) {
        logBox.textContent = error instanceof Error ? error.message : String(error);
      }
    };

    append(context.host, [
      card(
        "Firmware",
        null,
        h(
          "dl",
          { class: "kv" },
          h("dt", {}, "Product"),
          h("dd", {}, `${status.product} ${status.firmware}`),
          h("dt", {}, "Telemetry schema"),
          h("dd", {}, status.telemetry_schema),
          h("dt", {}, "Chip"),
          h("dd", {}, `${status.chip_model} @ ${status.cpu_mhz} MHz`),
          h("dt", {}, "Uptime"),
          h("dd", {}, duration(Math.floor(status.uptime_ms / 1000))),
          h("dt", {}, "Reset reason"),
          h("dd", {}, `${status.reset_reason} (boot #${status.boot_count})`),
          h("dt", {}, "Configuration"),
          h("dd", {}, status.config_status),
        ),
      ),
      card(
        "Memory and storage",
        null,
        h(
          "dl",
          { class: "kv" },
          h("dt", {}, "Free heap"),
          h("dd", {}, `${bytes(status.free_heap)} of ${bytes(status.heap_size)}`),
          h("dt", {}, "Lowest free heap"),
          h("dd", {}, bytes(status.min_free_heap)),
          h("dt", {}, "Sketch"),
          h("dd", {}, `${bytes(status.sketch_size)} used, ${bytes(status.free_sketch_space)} free`),
          h("dt", {}, "Filesystem"),
          h("dd", {}, `${bytes(status.fs_used)} of ${bytes(status.fs_total)}`),
        ),
        meter(status.fs_total ? (status.fs_used / status.fs_total) * 100 : 0, 80, 95),
      ),
      card(
        "Transport counters",
        null,
        h(
          "dl",
          { class: "kv" },
          h("dt", {}, "Active"),
          h("dd", {}, status.transport.active),
          h("dt", {}, "HTTP"),
          h("dd", {}, `${status.transport.http_polls} polls, ${status.transport.http_failures} failures`),
          h("dt", {}, "MQTT"),
          h("dd", {}, `${status.transport.mqtt_connected ? "connected" : "disconnected"}, ${status.transport.mqtt_messages} messages`),
          h("dt", {}, "mDNS"),
          h("dd", {}, status.transport.mdns_running ? "running" : "stopped"),
          h("dt", {}, "Log console"),
          h("dd", {}, `${status.log_console_clients} client(s)`),
        ),
      ),
      card("Log", "The last few kilobytes of the panel's own log ring.", h("div", { class: "row" }, button("Refresh", () => void loadLog())), logBox),
    ]);

    void loadLog();
  },
};

// =============================================================== firmware

const firmware: PageDefinition = {
  id: "firmware",
  title: "Firmware update",
  hint: "Upload a new firmware.bin. The panel writes the inactive slot and only switches over after it verifies.",
  async render(context) {
    heading(context, firmware);
    const file = h("input", { type: "file", accept: ".bin" });
    const progress = h("div", { class: "meter" }, h("span", { style: "width:0%" }));
    const label = h("p", { class: "hint" }, "No upload in progress.");

    append(context.host, [
      card(
        "Upload firmware",
        "Build it with: pio run -e cyd. The file is .pio/build/cyd/firmware.bin.",
        field("Firmware image", file),
        progress,
        label,
        button(
          "Upload and reboot",
          async () => {
            const chosen = file.files?.[0];
            if (!chosen) {
              toast("Choose a firmware.bin first", "error");
              return;
            }
            if (!confirmAction(`Flash ${chosen.name} (${bytes(chosen.size)})? The panel restarts afterwards.`)) return;
            try {
              await api.uploadFirmware(chosen, (percentDone) => {
                (progress.firstElementChild as HTMLElement).style.width = `${percentDone}%`;
                label.textContent = `Uploading… ${percentDone}%`;
              });
              label.textContent = "Upload complete. The panel is restarting.";
              toast("Firmware uploaded - restarting");
            } catch (error) {
              label.textContent = "Upload failed.";
              toast(error instanceof Error ? error.message : String(error), "error");
            }
          },
          "primary",
        ),
      ),
      card(
        "Over the air from PlatformIO",
        null,
        h("p", {}, "With OTA enabled on the Security page:"),
        h("pre", { class: "log" }, "pio run -e cyd-ota -t upload --upload-port wikistats.local"),
      ),
    ]);
  },
};

// ================================================================= backup

const backup: PageDefinition = {
  id: "backup",
  title: "Backup and restore",
  hint: "Export the configuration as JSON, or restore one. Secrets are excluded unless you ask for them.",
  async render(context) {
    heading(context, backup);
    const restoreArea = h("textarea", { rows: 10, placeholder: "Paste a backup document here" });

    append(context.host, [
      card(
        "Backup",
        "The plain export replaces every password, token and hash with a placeholder. It is safe to store or email.",
        h(
          "div",
          { class: "row" },
          h("a", { class: "btn", href: api.backupUrl(false), download: "wikistats-backup.json" }, "Download backup"),
          h(
            "a",
            { class: "btn danger", href: api.backupUrl(true), download: "wikistats-backup-secrets.json" },
            "Download with secrets",
          ),
        ),
        h(
          "p",
          { class: "hint", style: "margin-top:12px" },
          "The second file contains Wi-Fi passwords, the MQTT password, API tokens and the admin password hash in clear text. Treat it as a credential.",
        ),
      ),
      card(
        "Restore",
        "A backup without secrets keeps the passwords already on the panel rather than clearing them.",
        field("Backup JSON", restoreArea as unknown as HTMLElement),
        button(
          "Restore",
          async () => {
            if (!restoreArea.value.trim()) {
              toast("Paste a backup document first", "error");
              return;
            }
            if (!confirmAction("Replace the current configuration with this backup?")) return;
            try {
              await api.restore(restoreArea.value);
              toast("Configuration restored");
              await context.refreshShell();
            } catch (error) {
              toast(error instanceof Error ? error.message : String(error), "error");
            }
          },
          "primary",
        ),
      ),
    ]);
  },
};

// ============================================================ maintenance

const maintenance: PageDefinition = {
  id: "maintenance",
  title: "Restart and factory reset",
  hint: "Restarting keeps everything. A factory reset does not.",
  async render(context) {
    heading(context, maintenance);
    append(context.host, [
      card(
        "Restart",
        "Settings, devices and saved networks are kept.",
        button(
          "Restart the panel",
          async () => {
            if (!confirmAction("Restart the panel now?")) return;
            await api.restart();
            toast("Restarting - this page will reconnect in a few seconds");
          },
          "primary",
        ),
      ),
      card(
        "Factory reset",
        "Erases the device list, every saved Wi-Fi network, the MQTT settings and the administrator password.",
        button(
          "Erase everything",
          async () => {
            if (!confirmAction("Erase all settings? The panel returns to its first-boot setup portal.")) return;
            if (!confirmAction("This cannot be undone. Continue?")) return;
            await api.factoryReset();
            toast("Erased - the panel is restarting");
          },
          "danger",
        ),
      ),
    ]);
  },
};

export const pages: PageDefinition[] = [
  dashboard,
  devices,
  discovered,
  display,
  carousel,
  wifi,
  transport,
  thresholds,
  security,
  system,
  firmware,
  backup,
  maintenance,
];
