// Re-export of the UIA normalizer so vitest specs under `src/**/__tests__/`
// can import it via the `@/lib/...` alias. The implementation lives under
// `tests/helpers/` because the harness that produces UIA snapshots also
// lives under `tests/` (Playwright-driven) — keeping the normalizer next
// to its primary consumer avoids a phantom dependency from production
// code into snapshot tooling.

export {
  normalize,
  type UIANode,
  type Allowlist,
} from "../../tests/helpers/a11y-normalizer";
