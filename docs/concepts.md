# Concepts

## The rays

Each `bau_simulate` fires `rays` rays from the listener, spread evenly over a sphere (a Fibonacci sphere) and given a random turn each time. Each ray bounces up to `bounces` times. At each bounce:

- The surface absorbs part of the ray's energy, separately in three bands (low ~250 Hz, mid ~1 kHz, high ~4 kHz), using the material's `absorption`.
- The ray carries on in a direction between a mirror reflection and a random diffuse one. The material's `scattering` sets where between them.
- **Each source the bounce point can see** adds a path, using next-event estimation. The path's energy is what the ray still carries, times a diffuse-reflection term: `4 cos θ d² / (N r²)`. Here `r` is the distance from the bounce to the source and `d` is the straight distance from the listener to the source, so the result is relative to hearing the source with nothing in the way. The path's length becomes its delay, and its **arrival direction is the direction the ray first left the listener**.
- **If the bounce point can see the listener**, its energy goes into the echo: a histogram of energy against time (5 ms bins, up to 2 s).

Separately, each source gets a straight line to the listener. If solid cells are in the way, each one multiplies what gets through by its material's `transmission`, per band.

Every published value is blended with the previous one (35% new), so the results build up over calls and don't jitter.

## Muffle

`muffle` is 1 minus the square root of the high band's intensity reaching the listener, through walls and by every path the rays found, relative to hearing the source with nothing in the way:

- In plain sight: about 0, rising slightly with distance as the air absorbs highs.
- Behind a concrete wall with nothing round it: close to 1.
- Round a corner: between the two, depending on how many rays found it and what they bounced off.

At render time, muffle sets a low-pass cutoff from 20 kHz down to 300 Hz, using two one-pole stages (12 dB per octave). Gain comes from the mid band.

`reach` is the fraction of the listener's rays that found the source.

## Direction

A source is heard from the energy-weighted average of its paths' arrival directions. The straight path's direction counts at the strength of the sound that got through.

- When the source is in sight, the straight path dominates and it is heard straight at it.
- When it is behind a wall, the straight path is weak, so the rays that went out toward a corner and bounced round it dominate. It is heard from the corner.

`spread` is 1 minus the length of the averaged vector: 0 when everything comes from one way, near 1 when it comes from all round. Spread widens the panning toward every speaker.

## Echo

The listener's echo is worked out from its histogram and from statistics of all the rays:

- **enclosure:** of the rays that bounced, the fraction that hit something again rather than escaping to open sky.
- **free_path:** the mean distance between bounces.
- **decay (RT60):** from Eyring's formula. Each bounce keeps `(1 − mean absorption) × (1 − escape rate)` of the energy, and a bounce happens every free path. RT60 is the time taken to lose 60 dB.
- **level:** the energy that came back after the first 25 ms, mapped to 0..1 and scaled by enclosure. Early ground reflections don't count as echo.
- **taps:** up to four peaks in the histogram past 60 ms that stand well clear of what's around them. A far wall or a cliff gives one.
- **direction and focus:** where most of the returns came from.

## Rendering

`bau_render` pulls each source's block through its callback, then processes it in this order:

1. **Muffle:** the low-pass described above.
2. **Delay:** a ring buffer read at the path delay. The delay eases toward its target by at most 5% speed, which gives Doppler. A change of more than a quarter second jumps instead.
3. **Gain:** smoothed over about 10 ms. A new source starts at its target gain, so its own attack is kept.
4. **Panning:**
   - *Speakers* (quad, 5.1, 7.1): pairwise panning between the two speakers either side of the direction, keeping power, then blended toward every speaker equally by spread. The LFE channel is left silent.
   - *Stereo:* equal-power left/right from how far right the sound is.
   - *Headphones:* the same panning, plus an interaural delay on the far ear (Woodworth's model, up to ~0.66 ms), a head-shadow low-pass on the far ear, and a gentle dulling of both ears for sounds behind.
5. **Reverb send:** each source feeds the reverb as heard. The reverb is an 8-line feedback delay network: Householder feedback, damped lines, line gains set by RT60, and its energy normalised. Each channel reads the lines through a different Hadamard row so no two channels are alike, leaning toward the echo's direction. Taps are separate delays, each panned from its own direction.

## Threading

| Call | Thread |
|---|---|
| `bau_simulate` | Any one thread, usually a worker |
| `bau_render` | Any one thread, usually the audio device's |
| Everything else | The game's thread(s) |

Internally:

- A **world lock** is held by `bau_simulate` while it traces, and by grid creation, updates and destruction. Avoid editing grids every frame; moving them with `bau_grid_set_pose` is cheap and doesn't wait.
- A **render lock** is held by `bau_render`, source creation and source destruction. After `bau_source_destroy` returns, its callback is never called again.
- A **state lock** guards positions and published results. It is only ever held briefly.

## Coordinates

Coordinates are right-handed with y up. A grid's cells run x fastest: `cells[x + nx * (y + ny * z)]`. A grid's pose places cell 0's low corner at `origin` and turns the grid `yaw` radians about y, around that corner. A positive yaw turns +x toward −z.
