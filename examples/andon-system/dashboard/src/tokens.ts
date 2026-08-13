// Color tokens lifted from design.md §5.1 - reused here so the dashboard
// reads as the same visual system as the terminal firmware, per
// agents.md §11 ("keep operator terminal wording consistent with HMI
// labels") and PLAN.md §4.1's note on why the alert banner/category
// badges reuse these specific hex values rather than picking new ones.

export const COLORS = {
  bgBase: "#07131C",
  bgPanel: "#10212C",
  bgRaised: "#162A36",
  border: "#3E535F",
  textPrimary: "#F4F7FA",
  textSecondary: "#AFC0C9",
  info: "#00A8F3",
  running: "#35C759",
  waiting: "#F2A914",
  fault: "#EF3E3E",
  quality: "#7B4BC4",
  material: "#1976D2",
  disabled: "#52636C",
} as const;

// Matches firmware's CATEGORIES array (main.cpp) - MAINTENANCE=fault,
// QUALITY=quality, MATERIAL=material, SUPERVISOR=waiting.
export const CATEGORY_COLORS: Record<string, string> = {
  MAINTENANCE: COLORS.fault,
  QUALITY: COLORS.quality,
  MATERIAL: COLORS.material,
  SUPERVISOR: COLORS.waiting,
};

export const STATUS_COLORS: Record<string, string> = {
  OPEN: COLORS.fault,
  ACKNOWLEDGED: COLORS.waiting,
  HANDLING: COLORS.waiting,
  RESOLVED: COLORS.running,
};
