// ThemeToggle — toolbar widget for switching between dark and light
// themes. Persists choice to localStorage; sets data-theme on <html>.
//
// Default behavior at first launch (no stored value): reads OS-level
// prefers-color-scheme via matchMedia and applies that as initial state.
// Once the user explicitly toggles, the stored choice wins.

import { useEffect, useState } from "react";
import { Sun, Moon } from "lucide-react";

type Theme = "dark" | "light";

function readInitialTheme(): Theme {
  const stored = localStorage.getItem("alo:theme");
  if (stored === "dark" || stored === "light") return stored;
  return window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
}

export function ThemeToggle() {
  const [theme, setTheme] = useState<Theme>(() => readInitialTheme());

  useEffect(() => {
    document.documentElement.dataset.theme = theme;
  }, [theme]);

  const apply = (next: Theme) => {
    setTheme(next);
    localStorage.setItem("alo:theme", next);
  };

  return (
    <div className="inline-flex items-center bg-panel-2 border border-border-2 rounded-token-sm p-0.5">
      <button
        type="button"
        aria-label="Light theme"
        title="Light theme"
        aria-pressed={theme === "light"}
        onClick={() => apply("light")}
        className={`grid place-items-center w-6 h-5 rounded ${theme === "light" ? "bg-accent-soft text-accent" : "text-text-3"}`}
      >
        <Sun className="size-3.5" />
      </button>
      <button
        type="button"
        aria-label="Dark theme"
        title="Dark theme"
        aria-pressed={theme === "dark"}
        onClick={() => apply("dark")}
        className={`grid place-items-center w-6 h-5 rounded ${theme === "dark" ? "bg-accent-soft text-accent" : "text-text-3"}`}
      >
        <Moon className="size-3.5" />
      </button>
    </div>
  );
}
