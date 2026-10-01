import { useEffect, useLayoutEffect, useRef } from "react";
import { onUiMessage, type UiMessageOf, type UiMessageType } from "@/bridge/ui-message";

/** Run `handler` for every host ui/* push of `type` while mounted. The handler
 *  may close over fresh props/state: the latest one always runs, and the
 *  subscription itself only changes with `type`. */
export function useHostMessage<T extends UiMessageType>(
  type: T,
  handler: (msg: UiMessageOf<T>) => void,
): void {
  const latest = useRef(handler);
  useLayoutEffect(() => {
    latest.current = handler;
  });
  useEffect(() => onUiMessage(type, (msg) => latest.current(msg)), [type]);
}
