/// <reference types="vite/client" />

// Augment Vite's ImportMetaEnv with the build-time constants injected via
// vite.config.ts `define` so `import.meta.env.VITE_*` is typed at the
// usage site (e.g. AboutDialog reading VITE_APP_VERSION). Keep this list
// in sync with the `define` block.
interface ImportMetaEnv {
  readonly VITE_APP_VERSION: string;
  readonly VITE_BUILD_DATE: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
