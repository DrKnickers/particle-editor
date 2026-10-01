import type { PickerWhich } from "@/lib/record-control-messages";
import { useHostMessage } from "@/lib/use-host-message";

/** Keep a toolbar picker in sync with a record-mode ui/open-picker push. */
export function useOpenPickerMessage(which: PickerWhich, setOpen: (open: boolean) => void) {
  useHostMessage("ui/open-picker", (msg) => {
    if (msg.which === which) setOpen(msg.open);
  });
}
