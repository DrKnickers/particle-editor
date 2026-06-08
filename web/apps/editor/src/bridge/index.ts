import type { Bridge } from "@particle-editor/bridge-schema";
import { NativeBridge } from "./native";
import { MockBridge } from "./mock";

export function makeBridge(): Bridge {
  if (typeof window !== "undefined" && window.chrome?.webview) {
    return new NativeBridge();
  }
  return new MockBridge();
}
