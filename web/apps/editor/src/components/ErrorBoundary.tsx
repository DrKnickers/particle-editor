// ErrorBoundary — top-level crash guard for the editor shell.
//
// A render error anywhere under <App/> would otherwise leave the user
// staring at a blank WebView with no recourse. This boundary catches it,
// logs the error (so host.log / devtools capture the stack), and shows a
// minimal "Something went wrong" fallback with a Reload button.
//
// It also installs window-level `error` / `unhandledrejection` loggers so
// async failures that escape React's render path (a rejected bridge promise
// with no `.catch`, a setTimeout throw) still land in the console rather
// than vanishing silently. Those are logged only — they don't trip the
// fallback, which is reserved for render-time errors React hands us.

import { Component, type ErrorInfo, type ReactNode } from "react";

type Props = { children: ReactNode };
type State = { hasError: boolean };

export class ErrorBoundary extends Component<Props, State> {
  state: State = { hasError: false };

  static getDerivedStateFromError(): State {
    return { hasError: true };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    console.error("[ErrorBoundary] render error:", error, info.componentStack);
  }

  componentDidMount(): void {
    window.addEventListener("error", this.handleWindowError);
    window.addEventListener("unhandledrejection", this.handleRejection);
  }

  componentWillUnmount(): void {
    window.removeEventListener("error", this.handleWindowError);
    window.removeEventListener("unhandledrejection", this.handleRejection);
  }

  private handleWindowError = (e: ErrorEvent): void => {
    console.error("[ErrorBoundary] uncaught error:", e.error ?? e.message);
  };

  private handleRejection = (e: PromiseRejectionEvent): void => {
    console.error("[ErrorBoundary] unhandled rejection:", e.reason);
  };

  private handleReload = (): void => {
    window.location.reload();
  };

  render(): ReactNode {
    if (!this.state.hasError) return this.props.children;
    return (
      <div
        role="alert"
        className="flex h-full w-full flex-col items-center justify-center gap-3 p-6 text-center text-text"
      >
        <p className="text-sm font-medium">Something went wrong.</p>
        <button
          type="button"
          onClick={this.handleReload}
          className="rounded px-3 py-1 text-xs text-white transition motion-reduce:transition-none focus-ring bg-accent-strong hover:bg-accent-strong-hover"
        >
          Reload
        </button>
      </div>
    );
  }
}
