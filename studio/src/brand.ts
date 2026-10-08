// Central brand definition. Everything user-facing reads from here, so the
// product can be rebranded in exactly one place.
export const BRAND = {
  name: "Hexforge",
  product: "Hexforge Studio",
  wordmark: "HEXFORGE",
  sub: "STUDIO",
  version: "0.1.0",
  tagline: "Universal mod framework console",
  repo: "https://github.com/SpectralZero/Falcon_Hock",
} as const;

// Shared accent stops (kept in sync with styles.css --accent / --accent-2).
export const ACCENT = "#7C5CFF";
export const ACCENT_2 = "#35E0D0";
