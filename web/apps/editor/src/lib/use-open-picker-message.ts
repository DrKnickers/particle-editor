import { useEffect } from "react";
import { parseOpenPickerMessage, type PickerWhich } from "@/lib/record-focus-bridge";

/** Keep a toolbar picker in sync with a record-mode ui/open-picker push. */
export function useOpenPickerMessage(which: PickerWhich, setOpen: (open: boolean) => void) {
  useEffect(() => {
    const webview = window.chrome?.webview as
      | {
          addEventListener?: (event: string, listener: (event: { data: unknown }) => void) => void;
          removeEventListener?: (event: string, listener: (event: { data: unknown }) => void) => void;
        }
      | undefined;
    if (!webview?.addEventListener) return;
    const onMessage = (event: { data: unknown }) => {
      const message = parseOpenPickerMessage(event.data);
      if (message?.which === which) setOpen(message.open);
    };
    webview.addEventListener("message", onMessage);
    return () => webview.removeEventListener?.("message", onMessage);
  }, [which, setOpen]);
}
