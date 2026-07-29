// Minimal DOM helpers.
//
// No framework: the whole configuration UI is a few hundred lines of DOM building,
// and a runtime would add tens of kilobytes to a bundle that has to fit next to the
// firmware on a 4 MB flash chip.

type Attrs = Record<string, string | number | boolean | EventListener | undefined | null>;
type Child = Node | string | number | null | undefined | false;

export function h<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  attrs: Attrs = {},
  ...children: Child[]
): HTMLElementTagNameMap[K] {
  const element = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (value === undefined || value === null || value === false) continue;
    if (key.startsWith("on") && typeof value === "function") {
      element.addEventListener(key.slice(2).toLowerCase(), value as EventListener);
    } else if (key === "class") {
      element.className = String(value);
    } else if (key === "value" && element instanceof HTMLInputElement) {
      element.value = String(value);
    } else if (key === "checked" && element instanceof HTMLInputElement) {
      element.checked = Boolean(value);
    } else if (value === true) {
      element.setAttribute(key, "");
    } else {
      element.setAttribute(key, String(value));
    }
  }
  append(element, children);
  return element;
}

export function append(parent: Node, children: Child[]): void {
  for (const child of children) {
    if (child === null || child === undefined || child === false) continue;
    parent.appendChild(typeof child === "object" ? child : document.createTextNode(String(child)));
  }
}

export function clear(node: Element): void {
  while (node.firstChild) node.removeChild(node.firstChild);
}

export function card(title: string, hint: string | null, ...children: Child[]): HTMLElement {
  return h(
    "section",
    { class: "card" },
    h("h2", {}, title),
    hint ? h("p", { class: "hint" }, hint) : null,
    ...children,
  );
}

export function field(
  label: string,
  control: HTMLElement,
  help?: string,
): HTMLElement {
  const id = `f${Math.random().toString(36).slice(2, 9)}`;
  control.id = id;
  return h(
    "div",
    { class: "field" },
    h("label", { for: id }, label),
    control,
    help ? h("span", { class: "help" }, help) : null,
  );
}

export function textInput(value: string, attrs: Attrs = {}): HTMLInputElement {
  return h("input", { type: "text", value, ...attrs });
}

export function numberInput(value: number, min: number, max: number, attrs: Attrs = {}): HTMLInputElement {
  return h("input", { type: "number", value: String(value), min, max, ...attrs });
}

export function checkbox(checked: boolean, label: string, attrs: Attrs = {}): HTMLLabelElement {
  const input = h("input", { type: "checkbox", checked, ...attrs });
  return h("label", { class: "switch" }, input, h("span", {}, label));
}

export function select(
  value: string,
  options: { value: string; label: string }[],
  attrs: Attrs = {},
): HTMLSelectElement {
  const element = h(
    "select",
    attrs,
    ...options.map((option) =>
      h("option", { value: option.value, selected: option.value === value }, option.label),
    ),
  );
  element.value = value;
  return element;
}

export function button(label: string, onClick: () => void, variant = ""): HTMLButtonElement {
  return h("button", { class: `btn ${variant}`.trim(), onClick }, label);
}

let toastHost: HTMLElement | null = null;

export function toast(message: string, kind: "ok" | "error" = "ok"): void {
  if (!toastHost) {
    toastHost = h("div", { class: "toasts" });
    document.body.appendChild(toastHost);
  }
  const node = h("div", { class: `toast ${kind === "error" ? "error" : ""}`.trim() }, message);
  toastHost.appendChild(node);
  // Removed rather than hidden: a long session should not accumulate dead nodes.
  setTimeout(() => node.remove(), kind === "error" ? 6000 : 3000);
}

export function confirmAction(question: string): boolean {
  return window.confirm(question);
}
