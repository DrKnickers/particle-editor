import { describe, it, expect } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { Section } from "../Section";

describe("Section", () => {
  it("renders open by default with children visible", () => {
    render(<Section title="Emitter Timing"><div>child</div></Section>);
    expect(screen.getByText("Emitter Timing")).toBeInTheDocument();
    expect(screen.getByText("child")).toBeInTheDocument();
  });

  it("clicking the header collapses the section (animated wrapper flips data-open)", () => {
    // Post-animation: the body stays mounted and collapses via the
    // .collapse-anim grid + CSS visibility (jsdom can't see the CSS hide,
    // so we assert the state attribute that drives it). Content presence
    // is no longer the collapse signal.
    const { container } = render(<Section title="Generation"><div>child</div></Section>);
    const header = screen.getByTestId("section-generation");
    const anim = container.querySelector(".collapse-anim");
    expect(anim).toHaveAttribute("data-open", "true");
    fireEvent.click(header);
    expect(anim).toHaveAttribute("data-open", "false");
    expect(header).toHaveAttribute("aria-expanded", "false");
  });

  it("clicking again expands the section back", () => {
    render(<Section title="Forces"><div>child</div></Section>);
    const header = screen.getByTestId("section-forces");
    fireEvent.click(header);
    fireEvent.click(header);
    expect(screen.getByText("child")).toBeInTheDocument();
  });

  it("pressing Enter on the focused header toggles", () => {
    render(<Section title="Render"><div>child</div></Section>);
    const header = screen.getByTestId("section-render");
    expect(header).toHaveAttribute("aria-expanded", "true");
    fireEvent.keyDown(header, { key: "Enter" });
    expect(header).toHaveAttribute("aria-expanded", "false");
  });

  it("pressing Space on the focused header toggles", () => {
    render(<Section title="Texture"><div>child</div></Section>);
    const header = screen.getByTestId("section-texture");
    expect(header).toHaveAttribute("aria-expanded", "true");
    fireEvent.keyDown(header, { key: " " });
    expect(header).toHaveAttribute("aria-expanded", "false");
  });

  it("aria-expanded reflects open/closed state", () => {
    render(<Section title="Color"><div>child</div></Section>);
    const header = screen.getByTestId("section-color");
    expect(header).toHaveAttribute("aria-expanded", "true");
    fireEvent.click(header);
    expect(header).toHaveAttribute("aria-expanded", "false");
  });

  it("collapsed state flips data-open on the section container (drives chevron rotation in CSS)", () => {
    // B1.3.2: rotation is driven by `[data-open="false"]` on .panel-section
    // (matching the `:not([open])` selector for native <details> in the
    // shared CSS). The .collapsed modifier class is gone.
    const { container } = render(<Section title="Collision"><div>child</div></Section>);
    const section = container.querySelector(".panel-section");
    expect(section).toHaveAttribute("data-open", "true");
    const header = screen.getByTestId("section-collision");
    fireEvent.click(header);
    expect(section).toHaveAttribute("data-open", "false");
  });

  it("respects defaultOpen=false", () => {
    const { container } = render(
      <Section title="Turbulence" defaultOpen={false}><div>child</div></Section>,
    );
    const header = screen.getByTestId("section-turbulence");
    expect(header).toHaveAttribute("aria-expanded", "false");
    expect(container.querySelector(".collapse-anim")).toHaveAttribute("data-open", "false");
  });
});
