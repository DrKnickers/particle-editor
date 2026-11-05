// ESLint for the web workspace: the stock JS and TypeScript recommended rules plus the two
// classic React hooks rules. Formatting is prettier's job (.prettierrc.json), not ESLint's.
import js from "@eslint/js";
import reactHooks from "eslint-plugin-react-hooks";
import globals from "globals";
import tseslint from "typescript-eslint";

export default tseslint.config(
  { ignores: ["**/node_modules/", "**/dist/", "**/coverage/", "**/test-results/", "**/playwright-report/"] },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  {
    // Of the React Compiler rules in react-hooks 7, immutability, globals and purity are on.
    // refs and set-state-in-effect stay off pending review: they flag ~100 existing sites.
    plugins: { "react-hooks": reactHooks },
    rules: {
      "react-hooks/rules-of-hooks": "error",
      "react-hooks/exhaustive-deps": "error",
      "react-hooks/immutability": "error",
      "react-hooks/globals": "error",
      "react-hooks/purity": "error",
      "@typescript-eslint/no-explicit-any": "warn",
      // House conventions: a leading underscore marks a deliberately unused binding,
      // Playwright fixtures that need none are written `({}, testInfo)`, empty case labels
      // grouped under a comment share one body, and `declare global { namespace ... }`
      // augments Playwright's matcher types.
      "@typescript-eslint/no-unused-vars": [
        "error",
        { argsIgnorePattern: "^_", varsIgnorePattern: "^_", destructuredArrayIgnorePattern: "^_" },
      ],
      "no-empty-pattern": ["error", { allowObjectPatternsAsParameters: true }],
      "no-fallthrough": ["error", { allowEmptyCase: true }],
      "@typescript-eslint/no-namespace": ["error", { allowDeclarations: true }],
    },
  },
  {
    languageOptions: { globals: { ...globals.browser, ...globals.node } },
  },
);
