// Vitest tests for ErrorBoundary — the top-level crash guard.
//
// A child that throws during render should trip the boundary and show the
// "Something went wrong" fallback + a Reload button, instead of propagating
// the error and blanking the app.

import { describe, it, expect, vi, afterEach } from "vitest";
import { render, screen } from "@testing-library/react";
import { ErrorBoundary } from "../ErrorBoundary";

function Boom(): never {
  throw new Error("kaboom");
}

afterEach(() => {
  vi.restoreAllMocks();
});

describe("ErrorBoundary", () => {
  it("renders children when nothing throws", () => {
    render(
      <ErrorBoundary>
        <p>healthy child</p>
      </ErrorBoundary>,
    );
    expect(screen.getByText("healthy child")).toBeInTheDocument();
  });

  it("renders the fallback when a child throws during render", () => {
    // React logs the caught error to console.error; silence it so the test
    // output stays clean while still asserting our own log fired.
    const errSpy = vi.spyOn(console, "error").mockImplementation(() => {});
    render(
      <ErrorBoundary>
        <Boom />
      </ErrorBoundary>,
    );
    expect(screen.getByText("Something went wrong.")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Reload" })).toBeInTheDocument();
    // Our componentDidCatch logged the error (alongside React's own logs).
    expect(errSpy).toHaveBeenCalled();
  });

  it("Reload button calls window.location.reload", async () => {
    const userEvent = (await import("@testing-library/user-event")).default;
    vi.spyOn(console, "error").mockImplementation(() => {});
    const reload = vi.fn();
    // window.location.reload is read-only in jsdom; redefine via the property.
    Object.defineProperty(window, "location", {
      configurable: true,
      value: { ...window.location, reload },
    });
    render(
      <ErrorBoundary>
        <Boom />
      </ErrorBoundary>,
    );
    await userEvent.click(screen.getByRole("button", { name: "Reload" }));
    expect(reload).toHaveBeenCalledTimes(1);
  });
});
