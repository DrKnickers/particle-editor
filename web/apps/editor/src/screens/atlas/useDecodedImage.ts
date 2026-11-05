// Owns decoded images for imperative canvas painters, not texture fetching or display.
import { useEffect, useRef, useState } from "react";

export function useDecodedImage(dataUri: string | null) {
  const imageRef = useRef<HTMLImageElement | null>(null);
  const imageUriRef = useRef<string | null>(null); // dataUri the loaded img belongs to
  const [imageReady, setImageReady] = useState(false);

  useEffect(() => {
    if (!dataUri) {
      imageRef.current = null;
      imageUriRef.current = null;
      // eslint-disable-next-line react-hooks/set-state-in-effect -- Readiness must follow the Image decoder and the image ref consumed by canvas effects.
      setImageReady(false);
      return;
    }
    if (imageUriRef.current === dataUri && imageRef.current) {
      setImageReady(true);
      return;
    }
    let live = true;
    setImageReady(false);
    const img = new Image();
    img.src = dataUri;
    const ready = () => {
      if (!live) return;
      imageRef.current = img;
      imageUriRef.current = dataUri;
      setImageReady(true);
    };
    if (typeof img.decode === "function")
      img
        .decode()
        .then(ready)
        .catch(() => {
          /* fall back to onload */
        });
    img.onload = ready;
    return () => {
      live = false;
    };
  }, [dataUri]);

  return { imageRef, imageReady };
}
