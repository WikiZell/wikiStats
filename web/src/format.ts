// Presentation helpers. Deliberately the same rules the firmware uses in
// lib/fleet_core/fp_units.cpp, so the browser and the panel never disagree about
// what "3.0 GiB" means.

const BINARY = ["B", "KiB", "MiB", "GiB", "TiB", "PiB"];

export function bytes(value: number | undefined | null): string {
  if (value === undefined || value === null || !Number.isFinite(value) || value < 0) return "--";
  let scaled = value;
  let index = 0;
  while (scaled >= 1024 && index < BINARY.length - 1) {
    scaled /= 1024;
    index += 1;
  }
  if (index === 0) return `${Math.round(scaled)} ${BINARY[index]}`;
  return `${scaled < 100 ? scaled.toFixed(1) : Math.round(scaled)} ${BINARY[index]}`;
}

export function rate(value: number | undefined | null): string {
  const text = bytes(value);
  return text === "--" ? text : `${text}/s`;
}

export function percent(value: number | undefined | null, decimals = 0): string {
  if (value === undefined || value === null || !Number.isFinite(value)) return "--";
  return `${value.toFixed(decimals)}%`;
}

export function celsius(value: number | undefined | null): string {
  if (value === undefined || value === null || !Number.isFinite(value)) return "--";
  return `${value.toFixed(1)} °C`;
}

export function duration(seconds: number | undefined | null): string {
  if (seconds === undefined || seconds === null || !Number.isFinite(seconds) || seconds < 0) {
    return "--";
  }
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const secs = Math.floor(seconds % 60);
  if (days > 0) return `${days}d ${hours}h`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  if (minutes > 0) return `${minutes}m ${secs}s`;
  return `${secs}s`;
}

export function age(seconds: number | undefined | null): string {
  if (seconds === undefined || seconds === null) return "--";
  if (seconds < 2) return "now";
  return duration(seconds);
}

export function signal(rssi: number): string {
  if (rssi >= -55) return "excellent";
  if (rssi >= -67) return "good";
  if (rssi >= -75) return "fair";
  return "weak";
}
