// 3D value-gradient noise, in-tree.
//
// TopographicBackground previously got this from
// `import * as ChriscoursesPerlinNoise from "https://esm.sh/@chriscourses/perlin-noise"`.
// A remote ES module import is wrong here three times over: Gravity is a local-first
// desktop app that must work with no network at all; the Tauri window's CSP is
// `default-src 'self'` (tauri.conf.json), which blocks the fetch outright, taking the
// whole entry-point module graph -- and therefore the entire UI -- down with it; and
// `tsc --noEmit` cannot resolve a URL specifier, so it broke `npm run build`.
//
// This is the same algorithm that package implements (p5.js's `noise()`): a lattice of
// pseudo-random values, sampled with cosine interpolation and summed over octaves with a
// halving amplitude. Keeping the algorithm identical keeps the background looking the way
// it was designed to look.
//
// The lattice is filled from a seeded PRNG rather than Math.random() so a given build
// always draws the same field -- one less thing that can differ between two runs.

const PERLIN_YWRAPB = 4;
const PERLIN_YWRAP = 1 << PERLIN_YWRAPB;
const PERLIN_ZWRAPB = 8;
const PERLIN_ZWRAP = 1 << PERLIN_ZWRAPB;
const PERLIN_SIZE = 4095;

const OCTAVES = 4;
const AMP_FALLOFF = 0.5;

// mulberry32: tiny, fast, well-distributed enough for a decorative noise lattice.
function seededRandom(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const lattice = (() => {
  const random = seededRandom(0x9e3779b9);
  const values = new Float64Array(PERLIN_SIZE + 1);
  for (let i = 0; i < values.length; i++) values[i] = random();
  return values;
})();

function scaledCosine(i: number): number {
  return 0.5 * (1.0 - Math.cos(i * Math.PI));
}

/** Smooth noise in [0, 1). Coordinates may be any finite numbers; negatives are mirrored. */
export function noise(x: number, y = 0, z = 0): number {
  let xi = Math.floor(Math.abs(x));
  let yi = Math.floor(Math.abs(y));
  let zi = Math.floor(Math.abs(z));
  let xf = Math.abs(x) - xi;
  let yf = Math.abs(y) - yi;
  let zf = Math.abs(z) - zi;

  let result = 0;
  let amplitude = 0.5;

  for (let octave = 0; octave < OCTAVES; octave++) {
    let of = xi + (yi << PERLIN_YWRAPB) + (zi << PERLIN_ZWRAPB);

    const rxf = scaledCosine(xf);
    const ryf = scaledCosine(yf);

    let n1 = lattice[of & PERLIN_SIZE];
    n1 += rxf * (lattice[(of + 1) & PERLIN_SIZE] - n1);
    let n2 = lattice[(of + PERLIN_YWRAP) & PERLIN_SIZE];
    n2 += rxf * (lattice[(of + PERLIN_YWRAP + 1) & PERLIN_SIZE] - n2);
    n1 += ryf * (n2 - n1);

    of += PERLIN_ZWRAP;
    let n3 = lattice[of & PERLIN_SIZE];
    n3 += rxf * (lattice[(of + 1) & PERLIN_SIZE] - n3);
    let n4 = lattice[(of + PERLIN_YWRAP) & PERLIN_SIZE];
    n4 += rxf * (lattice[(of + PERLIN_YWRAP + 1) & PERLIN_SIZE] - n4);
    n3 += ryf * (n4 - n3);

    n1 += scaledCosine(zf) * (n3 - n1);

    result += n1 * amplitude;
    amplitude *= AMP_FALLOFF;

    xi <<= 1;
    xf *= 2;
    yi <<= 1;
    yf *= 2;
    zi <<= 1;
    zf *= 2;

    if (xf >= 1.0) {
      xi++;
      xf--;
    }
    if (yf >= 1.0) {
      yi++;
      yf--;
    }
    if (zf >= 1.0) {
      zi++;
      zf--;
    }
  }

  return result;
}
