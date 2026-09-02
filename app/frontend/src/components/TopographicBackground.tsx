import { useEffect, useRef } from "react";
import { noise } from "../utils/perlinNoise";
import { useTheme } from "../context/ThemeContext";
import styles from "./TopographicBackground.module.css";

export default function TopographicBackground() {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const { colors } = useTheme();

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    // Editable values
    const thresholdIncrement = 5;
    const thickLineThresholdMultiple = 3;
    const res = 8;
    const baseZOffset = 0.0001;
    const lineColor = colors.topoLineColor;

    let inputValues: number[][] = [];
    let currentThreshold = 0;
    let cols = 0;
    let rows = 0;
    let zOffset = 0;
    let zBoostValues: number[][] = [];
    let noiseMin = 100;
    let noiseMax = 0;
    let mousePos = { x: -99, y: -99 };
    // The one thing that must be cancellable on unmount: without this the requestAnimationFrame
    // chain below outlived the component, drawing forever into a detached canvas.
    let frameHandle = 0;

    const canvasSize = () => {
      const rect = canvas.parentElement?.getBoundingClientRect() || canvas.getBoundingClientRect();
      canvas.width = rect.width * window.devicePixelRatio;
      canvas.height = rect.height * window.devicePixelRatio;
      // setTransform, not scale(): scale() COMPOUNDS with whatever transform is already on
      // the context, so every resize used to multiply the device-pixel-ratio scale again
      // and the field zoomed in a step at a time.
      ctx.setTransform(window.devicePixelRatio, 0, 0, window.devicePixelRatio, 0, 0);
      canvas.style.width = rect.width + "px";
      canvas.style.height = rect.height + "px";
      cols = Math.floor(canvas.width / res) + 1;
      rows = Math.floor(canvas.height / res) + 1;

      zBoostValues = [];
      for (let y = 0; y < rows; y++) {
        zBoostValues[y] = [];
        for (let x = 0; x <= cols; x++) {
          zBoostValues[y][x] = 0;
        }
      }
    };

    const linInterpolate = (x0: number, x1: number, y0 = 0, y1 = 1) => {
      if (x0 === x1) return 0;
      return y0 + ((y1 - y0) * (currentThreshold - x0)) / (x1 - x0);
    };

    const binaryToType = (nw: number, ne: number, se: number, sw: number) => {
      const a = [nw, ne, se, sw];
      return a.reduce((res, x) => (res << 1) | x);
    };

    const line = (from: number[], to: number[]) => {
      ctx.moveTo(from[0], from[1]);
      ctx.lineTo(to[0], to[1]);
    };

    const placeLines = (gridValue: number, x: number, y: number) => {
      const nw = inputValues[y][x];
      const ne = inputValues[y][x + 1];
      const se = inputValues[y + 1][x + 1];
      const sw = inputValues[y + 1][x];
      let a, b, c, d;

      switch (gridValue) {
        case 1:
        case 14:
          c = [x * res + res * linInterpolate(sw, se), y * res + res];
          d = [x * res, y * res + res * linInterpolate(nw, sw)];
          line(d, c);
          break;
        case 2:
        case 13:
          b = [x * res + res, y * res + res * linInterpolate(ne, se)];
          c = [x * res + res * linInterpolate(sw, se), y * res + res];
          line(b, c);
          break;
        case 3:
        case 12:
          b = [x * res + res, y * res + res * linInterpolate(ne, se)];
          d = [x * res, y * res + res * linInterpolate(nw, sw)];
          line(d, b);
          break;
        case 11:
        case 4:
          a = [x * res + res * linInterpolate(nw, ne), y * res];
          b = [x * res + res, y * res + res * linInterpolate(ne, se)];
          line(a, b);
          break;
        case 5:
          a = [x * res + res * linInterpolate(nw, ne), y * res];
          b = [x * res + res, y * res + res * linInterpolate(ne, se)];
          c = [x * res + res * linInterpolate(sw, se), y * res + res];
          d = [x * res, y * res + res * linInterpolate(nw, sw)];
          line(d, a);
          line(c, b);
          break;
        case 6:
        case 9:
          a = [x * res + res * linInterpolate(nw, ne), y * res];
          c = [x * res + res * linInterpolate(sw, se), y * res + res];
          line(c, a);
          break;
        case 7:
        case 8:
          a = [x * res + res * linInterpolate(nw, ne), y * res];
          d = [x * res, y * res + res * linInterpolate(nw, sw)];
          line(d, a);
          break;
        case 10:
          a = [x * res + res * linInterpolate(nw, ne), y * res];
          b = [x * res + res, y * res + res * linInterpolate(ne, se)];
          c = [x * res + res * linInterpolate(sw, se), y * res + res];
          d = [x * res, y * res + res * linInterpolate(nw, sw)];
          line(a, b);
          line(c, d);
          break;
        default:
          break;
      }
    };

    const renderAtThreshold = () => {
      ctx.beginPath();
      ctx.strokeStyle = lineColor;
      ctx.lineWidth = currentThreshold % (thresholdIncrement * thickLineThresholdMultiple) === 0 ? 2 : 1;

      for (let y = 0; y < inputValues.length - 1; y++) {
        for (let x = 0; x < inputValues[y].length - 1; x++) {
          if (
            inputValues[y][x] > currentThreshold &&
            inputValues[y][x + 1] > currentThreshold &&
            inputValues[y + 1][x + 1] > currentThreshold &&
            inputValues[y + 1][x] > currentThreshold
          )
            continue;
          if (
            inputValues[y][x] < currentThreshold &&
            inputValues[y][x + 1] < currentThreshold &&
            inputValues[y + 1][x + 1] < currentThreshold &&
            inputValues[y + 1][x] < currentThreshold
          )
            continue;

          const gridValue = binaryToType(
            inputValues[y][x] > currentThreshold ? 1 : 0,
            inputValues[y][x + 1] > currentThreshold ? 1 : 0,
            inputValues[y + 1][x + 1] > currentThreshold ? 1 : 0,
            inputValues[y + 1][x] > currentThreshold ? 1 : 0
          );

          placeLines(gridValue, x, y);
        }
      }
      ctx.stroke();
    };

    const generateNoise = () => {
      for (let y = 0; y < rows; y++) {
        inputValues[y] = [];
        for (let x = 0; x <= cols; x++) {
          inputValues[y][x] =
            noise(x * 0.02, y * 0.02, zOffset + (zBoostValues[y]?.[x] || 0)) * 100;
          if (inputValues[y][x] < noiseMin) noiseMin = inputValues[y][x];
          if (inputValues[y][x] > noiseMax) noiseMax = inputValues[y][x];
          if ((zBoostValues[y]?.[x] || 0) > 0) {
            zBoostValues[y][x] *= 0.95;
          }
        }
      }
    };

    const mouseOffset = () => {
      const x = Math.floor(mousePos.x / res);
      const y = Math.floor(mousePos.y / res);
      if (!inputValues[y] || inputValues[y][x] === undefined) return;
      const incrementValue = 0.008;
      const radius = 8;
      for (let i = -radius; i <= radius; i++) {
        for (let j = -radius; j <= radius; j++) {
          const distanceSquared = i * i + j * j;
          const radiusSquared = radius * radius;
          if (distanceSquared <= radiusSquared && zBoostValues[y + i]?.[x + j] !== undefined) {
            zBoostValues[y + i][x + j] += incrementValue * (1 - distanceSquared / radiusSquared);
          }
        }
      }
    };

    const animate = () => {
      // Scheduled first and kept in `frameHandle` so cleanup can cancel exactly the frame
      // that is actually pending. (This used to be a setTimeout wrapping a
      // requestAnimationFrame, with the handle of neither retained -- so nothing could stop
      // it, and the two schedulers each queued the next frame independently.)
      frameHandle = requestAnimationFrame(animate);

      mouseOffset();

      ctx.clearRect(0, 0, canvas.width, canvas.height);

      zOffset += baseZOffset;
      generateNoise();

      const roundedNoiseMin = Math.floor(noiseMin / thresholdIncrement) * thresholdIncrement;
      const roundedNoiseMax = Math.ceil(noiseMax / thresholdIncrement) * thresholdIncrement;
      for (let threshold = roundedNoiseMin; threshold < roundedNoiseMax; threshold += thresholdIncrement) {
        currentThreshold = threshold;
        renderAtThreshold();
      }
      noiseMin = 100;
      noiseMax = 0;
    };

    // Named handlers, because removeEventListener compares by identity: the previous
    // cleanup passed freshly-created arrow functions, which match nothing and removed
    // nothing, so every mount leaked its listeners onto window/canvas permanently.
    const handleResize = () => canvasSize();
    // On `window`, not on the canvas. The canvas sits at z-index -1 behind AppShell's
    // content layer (z-index 1), which covers the whole viewport -- so it was never the
    // hit-test target and never received a single mousemove, leaving the cursor-follows
    // effect dead on arrival. Listening at the window and converting client coordinates
    // into canvas-local ones is what makes it actually respond to the pointer, and lets
    // the canvas keep `pointer-events: none`, which is what a background should have.
    const handleMouseMove = (e: MouseEvent) => {
      const rect = canvas.getBoundingClientRect();
      mousePos = { x: e.clientX - rect.left, y: e.clientY - rect.top };
    };
    // Pointer gone from the window: park it somewhere the grid can't reach, or the last
    // position keeps boosting that spot forever.
    const handleMouseLeave = () => {
      mousePos = { x: -99, y: -99 };
    };

    canvasSize();
    window.addEventListener("resize", handleResize);
    window.addEventListener("mousemove", handleMouseMove);
    document.addEventListener("mouseleave", handleMouseLeave);

    animate();

    return () => {
      cancelAnimationFrame(frameHandle);
      window.removeEventListener("resize", handleResize);
      window.removeEventListener("mousemove", handleMouseMove);
      document.removeEventListener("mouseleave", handleMouseLeave);
    };
  }, [colors.topoLineColor]);

  return <canvas ref={canvasRef} className={styles.canvas} aria-hidden="true" />;
}
