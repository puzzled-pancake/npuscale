# NPUpscale

Release 0.0.1-alpha, called "First Contact".

NPUpscale runs an AI upscaler on the NPU of Snapdragon handhelds and hooks it
into games through the XeSS slot. The GPU hands over small frames and gets
big ones back. On a Snapdragon 8 Gen 2 class chip the upscale pass takes
about 3.4 ms per frame, which leaves the GPU free to work on the game.

It supports No Man's Sky.

This project is not affiliated with AMD or Qualcomm. Their files are never
bundled here. See the licensing section at the end.

## What you need

- A Snapdragon 8 Gen 2 class handheld (QCS8550 and the same chip family)
  running Armada OS. This is the only qualified device. The panel checks the
  chip and says so plainly if yours is not supported.
- No Man's Sky on Steam.
- Two download packs that you host yourself. The licenses on the AI models
  (AMD) and the NPU runtime (Qualcomm) forbid redistribution, so you put
  them on your own server once and point the plugin at them:

  | Pack | What is inside | File |
  |---|---|---|
  | Models | 12 files, five upscaler networks plus their weights | fsr4npu-models.tar.gz |
  | Runtime | 8 files, QNN libraries plus fastRPC | fsr4npu-runtime.tar.gz |

  A plain folder of loose files works too. Every file is checksum checked
  during install, so a broken download cannot install.

## Install

1. Unzip the plugin into `~/homebrew/plugins/` and restart Steam. On Armada
   that folder needs sudo, so one terminal command does it:
   `sudo unzip NPUpscale-*.zip -d ~/homebrew/plugins/`
2. Open the quick access menu, find NPUpscale, open No Man's Sky.
3. Under Licensed files, enter your two URLs, then press Install.
4. Launch the game. Nothing else. The upscaler turns itself on and the game
   settings pick the network.

## The panel, tab by tab

### Games

The top card answers one question: is upscaling running right now?

- Green, "Upscaling is running": the game is up and the NPU is working. It
  shows the time per frame and the frame rate ceiling that time implies.
- Green, "Ready to play": installed, game not running. Launch and play.
- Amber, "Finish setup": files missing or not installed yet. The button
  jumps to the game page.

Below that is the game list. Each game opens its own page. The donate row
sits under the list.

### The game page

- Game found: Steam has the game.
- Upscaler installed: the XeSS slot DLL is in place.
- NPU layer active: the Vulkan layer and its manifest are installed.
- Launch environment: the game will start with the right settings.

Install puts everything in place. Repair repeats the install and rechecks
every file. Remove takes the upscaler out of the game and puts the original
XeSS DLL back. Remove asks twice before it does anything.

### NPU

This tab is everything about the chip.

The chip card names your SoC (for example QCS8550), its NPU generation, and
whether this build supports it. If your chip needs a different generation,
the card says which one.

Licensed files shows the two packs and their counts. Change URLs opens the
two address fields. Save and download fetches both packs and checks them.

Voltage picks the NPU clock and voltage corner. There are six buttons:

| Button | Corner | What it means |
|---|---|---|
| MAX, stock | 0xA0 | Full speed. This is the default. About 3.4 ms per frame. |
| NOM | 0x60 | Nominal. Slower, less power drawn. |
| SVS+ | 0x50 | Between NOM and SVS. |
| SVS | 0x40 | Low. About 6 ms per frame on a 8 Gen 2. |
| SVS2 | 0x30 | Near the bottom. About 8 ms per frame. |
| MIN | 0x20 | Lowest setting. On current hardware this behaves the same as SVS2, because the chip stops going lower. |

Lower corners save battery and produce more heat per frame of work. A change
takes effect within a second, in game or not. Pick a corner, run the
benchmark, compare, repeat.

Benchmark runs the actual upscaler network on the NPU. It works with the
game closed, and it also reads the live numbers while you play. The result
shows the time per frame, the best and worst run, and the frame rate ceiling
the time allows. The progress bar fills while it measures.

### Settings

These are defaults for every game.

Exposure sets the light level of frames fed into the upscaler, from 0.5 to
8.0, default 4.0. It is not the game brightness and not the screen
brightness; those stay where the game and the display put them. This value
only matters when Auto brightness is off. When it is on, the upscaler
measures each frame and picks the exposure itself.

Sharpness goes from 0 to 1, default 0.5. High values make edges harder and
can add halos around them.

Edge smoothing is the blend floor for temporal smoothing, 0 to 1, default 0.
It controls how much of the previous frame the upscaler keeps when the new
one disagrees. Zero follows the reference implementation. Raising it calms
shimmer at the cost of some trailing on fast motion.

Smoothing hold is how fast that blend decays, 0 to 1, default 0.35. Higher
values hold the smoothing longer during motion.

Auto exposure turns automatic exposure on. The upscaler then measures the
scene each frame the way the reference implementation does. While it is on,
it overrides the Exposure setting. It takes effect at game start.

Motion clarity turns motion vector dilation on. This widens motion
information at object edges, which helps moving objects but can smear fine
detail. Off by default because it looked worse in testing on this game.

Reset to defaults puts every value back.

Advanced holds two items. Zero-copy memory streams frames to the NPU without
CPU copies; leave it on unless told otherwise. Save NPU debug dump writes one
frame of NPU input and output to disk for bug reports.

## Report a problem

Settings, Advanced, Report a problem builds a short text summary: versions,
chip, install state, live numbers, and the last error lines from the logs.
Nothing is sent anywhere. Copy report puts it on the clipboard and you paste
it wherever you ask for help.

## Game settings that ship with this project

The included graphics settings file for No Man's Sky uses the game's own low
preset for every detail option, which is what the game picks by itself on
this hardware. Two entries differ from the game default:

- Anti aliasing is set to XeSS, quality Performance. That is the slot the
  upscaler lives in, and Performance is the best tested network for this
  resolution. You can change quality in game at any time; each setting
  switches to its matching network automatically.
- HDR is off.

## Building from source

The panel is TypeScript, bundled with rollup. The backend is one Python
file. The native parts are C: the Vulkan layer builds with zig cc against
the QAIRT headers, the XeSS slot DLL builds with mingw, and the benchmark
tool builds the same way as the layer. See the build scripts in
`src-native/` for the exact commands. Payload binaries are not stored in
this repository.

## Licensing

The plugin code is Copyright puzzled-pancake and licensed under the GNU GPL
v3.0 or later. See LICENSE. You are free to use, study, and change it, and
anyone who distributes it or a derivative must publish their source.

The upscaler is a port of the AMD FidelityFX Super Resolution 4 network,
built from the FidelityFX SDK under its license. AMD and FidelityFX are
trademarks of AMD. The model files are not distributed here.

The NPU runtime is Qualcomm QAIRT SDK material under its license. Qualcomm
and Hexagon are trademarks of Qualcomm. The runtime files are not
distributed here.

If this makes your handheld better, you can
[sponsor development](https://github.com/sponsors/puzzled-pancake).
