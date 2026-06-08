// Design tokens — placeholders. Iterate in browser mode against
// tasks/lt4_design_parking_lot.md mockups before locking in.
export const tokens = {
  color: {
    bg: { app: "#0F1115", panel: "#16191F", surface: "#1C2028" },
    fg: { primary: "#E6E8EB", muted: "#8A9099", subtle: "#4A4F58" },
    accent: { primary: "#5BA3F5", danger: "#F56A6A", success: "#6AD08A" },
    border: { subtle: "#262A33", strong: "#3A3F4A" },
  },
  space: { 0: "0px", 1: "4px", 2: "8px", 3: "12px", 4: "16px", 6: "24px", 8: "32px" },
  radius: { sm: "4px", md: "6px", lg: "10px" },
  type: {
    family: { ui: "'Inter', system-ui, sans-serif", mono: "'JetBrains Mono', monospace" },
    size: { xs: "11px", sm: "12px", md: "13px", lg: "15px", xl: "18px" },
    weight: { regular: 400, medium: 500, semibold: 600 },
  },
  density: { rowHeight: { tight: "22px", default: "26px", loose: "32px" } },
} as const;

export type Tokens = typeof tokens;
