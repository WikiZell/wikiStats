// Application shell: session gate, sidebar, hash router.
//
// The gate has three states rather than two, because a panel that has never been
// configured has no password to sign in with:
//
//   * provisioning - first boot, setup portal is up: the Wi-Fi page is reachable
//     without a password, and a banner insists on setting one.
//   * locked       - a password exists and no session: show the login form.
//   * unlocked     - full navigation.

import "./styles.css";

import { api, onSessionChange, type Session, type Status } from "./api";
import { append, button, card, clear, field, h, toast } from "./dom";
import { pages, type PageContext, type PageDefinition } from "./pages";

const root = document.getElementById("app");
if (!root) throw new Error("#app is missing");

let session: Session = {
  authenticated: false,
  password_set: true,
  provisioning: false,
  web_enabled: true,
};
let status: Status | null = null;
let currentPage = "dashboard";
let refreshTimer: number | undefined;

onSessionChange((next) => {
  session = next;
});

function pageFor(id: string): PageDefinition {
  return pages.find((page) => page.id === id) ?? pages[0];
}

function readHash(): string {
  const id = window.location.hash.replace(/^#/, "");
  return pages.some((page) => page.id === id) ? id : "dashboard";
}

// --------------------------------------------------------------- rendering

function renderLogin(message?: string): void {
  clear(root!);
  const password = h("input", { type: "password", autocomplete: "current-password" });
  const submit = async (): Promise<void> => {
    try {
      await api.login(password.value);
      await boot();
    } catch (error) {
      toast(error instanceof Error ? error.message : String(error), "error");
      password.value = "";
      password.focus();
    }
  };
  password.addEventListener("keydown", (event) => {
    if ((event as KeyboardEvent).key === "Enter") void submit();
  });

  append(root!, [
    h(
      "div",
      { class: "login" },
      h("h1", {}, "WikiStats"),
      h("p", { class: "page-hint" }, message ?? "Sign in to configure this panel."),
      card("Administrator", null, field("Password", password), button("Sign in", () => void submit(), "primary")),
    ),
  ]);
  password.focus();
}

function renderFirstRun(): void {
  clear(root!);
  const password = h("input", { type: "password", autocomplete: "new-password" });
  const confirm = h("input", { type: "password", autocomplete: "new-password" });

  append(root!, [
    h(
      "div",
      { class: "login" },
      h("h1", {}, "Welcome to WikiStats"),
      h(
        "p",
        { class: "page-hint" },
        "Choose an administrator password before anything else. Until you do, this website refuses every settings change.",
      ),
      card(
        "Set a password",
        "At least 8 characters. It is stored only as a PBKDF2 hash.",
        field("Password", password),
        field("Repeat", confirm),
        button(
          "Set password",
          async () => {
            if (password.value !== confirm.value) {
              toast("The two passwords do not match", "error");
              return;
            }
            try {
              await api.setPassword(password.value);
              toast("Password set - sign in");
              await boot();
            } catch (error) {
              toast(error instanceof Error ? error.message : String(error), "error");
            }
          },
          "primary",
        ),
      ),
      h(
        "p",
        { class: "page-hint" },
        "Setting up Wi-Fi first? ",
        h(
          "a",
          {
            href: "#wifi",
            onClick: () => {
              currentPage = "wifi";
              renderShell();
            },
          },
          "Open Wi-Fi setup",
        ),
      ),
    ),
  ]);
  password.focus();
}

function renderShell(): void {
  clear(root!);

  const nav = h("nav", { class: "nav" });
  for (const page of pages) {
    const count = page.badge && status ? page.badge(status) : 0;
    const item = h(
      "button",
      {
        onClick: () => {
          window.location.hash = page.id;
        },
      },
      page.title,
      count > 0 ? h("span", { class: "nav-badge" }, String(count)) : null,
    );
    if (page.id === currentPage) item.setAttribute("aria-current", "page");
    nav.appendChild(item);
  }

  const sidebar = h(
    "aside",
    { class: "sidebar" },
    h("div", { class: "brand" }, "WikiStats", h("small", {}, status ? `firmware ${status.firmware}` : "")),
    nav,
    h(
      "div",
      { style: "margin-top:18px;padding:0 8px" },
      session.authenticated
        ? button("Sign out", async () => {
            await api.logout();
            await boot();
          })
        : null,
    ),
  );

  const main = h("main", { class: "main" });
  append(root!, [h("div", { class: "shell" }, sidebar, main)]);

  const context: PageContext = { host: main, refreshShell: boot };
  const page = pageFor(currentPage);
  void page.render(context).catch((error: unknown) => {
    clear(main);
    append(main, [
      h("h1", {}, page.title),
      card("Could not load this page", null, h("p", {}, error instanceof Error ? error.message : String(error))),
    ]);
  });
}

// ------------------------------------------------------------------- boot

async function boot(): Promise<void> {
  root!.removeAttribute("data-loading");
  try {
    session = await api.session();
  } catch (error) {
    clear(root!);
    append(root!, [
      h(
        "div",
        { class: "login" },
        h("h1", {}, "WikiStats"),
        card("Cannot reach the panel", null, h("p", {}, error instanceof Error ? error.message : String(error))),
      ),
    ]);
    return;
  }

  if (!session.web_enabled) {
    clear(root!);
    append(root!, [
      h(
        "div",
        { class: "login" },
        h("h1", {}, "WikiStats"),
        card(
          "Web configuration is disabled",
          null,
          h("p", {}, "Re-enable it from the panel's touchscreen, or perform a factory reset."),
        ),
      ),
    ]);
    return;
  }

  if (!session.password_set) {
    // First boot: let the user set a password, but keep Wi-Fi reachable so the
    // panel can be put on a network in the first place.
    if (currentPage !== "wifi") {
      renderFirstRun();
      return;
    }
  } else if (!session.authenticated) {
    renderLogin();
    return;
  }

  try {
    status = await api.status();
  } catch {
    status = null;
  }
  renderShell();

  // A slow poll keeps the sidebar badge and the dashboard honest without hammering
  // an ESP32 that is also polling half a dozen agents.
  window.clearInterval(refreshTimer);
  refreshTimer = window.setInterval(async () => {
    try {
      status = await api.status();
      if (currentPage === "dashboard") renderShell();
    } catch {
      /* transient; the next tick retries */
    }
  }, 10000);
}

window.addEventListener("hashchange", () => {
  currentPage = readHash();
  void boot();
});

currentPage = readHash();
void boot();
