# Decky LSFG-VK Adaptive

Fork of [xXJSONDeruloXx/decky-lsfg-vk](https://github.com/xXJSONDeruloXx/decky-lsfg-vk) for the **Adaptive** lsfg-vk engine in this repository (`redygreen/lsfg-vk`).

It is **not** a drop-in replacement for the official plugin. Paths, layer filenames, and the launch wrapper are isolated so both can be installed at once.

| | Official Decky LSFG-VK | This plugin |
|---|---|---|
| Layer files | `~/.local/lib/liblsfg-vk.so` / `~/.local/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation.json` | `~/.local/share/decky-lsfg-vk-adaptive/` (private tree, not on the default Vulkan search path) |
| Config | `~/.config/lsfg-vk/conf.toml` (v1 `[[game]]`) | `~/.config/lsfg-vk-adaptive/conf.toml` (v2 `[[profile]]`) |
| Launch option | `~/lsfg %command%` | `~/lsfg-vk-adaptive %command%` |

This matches how [decky-lsfg-vk-experimental](https://github.com/eugeniosegala/decky-lsfg-vk-experimental) avoids clashing with the official plugin: the layer is not installed into `~/.local/share/vulkan/implicit_layer.d`. The wrapper sets `VK_IMPLICIT_LAYER_PATH` so only this fork's layer is visible to the game, plus `DISABLE_LSFG=1` so official 1.x stays off if a nested loader still finds it.

## What it does

- Installs this fork's v2 Vulkan layer into `~/.local/share/decky-lsfg-vk-adaptive` without touching official 1.x files.
- On Install, also removes leftover Adaptive files from the global Vulkan path (`~/.local/lib/liblsfg-vk-layer.so` and `~/.local/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json`) if an earlier zip put them there.
- Writes Adaptive-aware v2 TOML (`adaptive`, `target_fps`, `allow_fp16`, `multiplier` as a ceiling).
- Creates `~/lsfg-vk-adaptive`, which sets `VK_IMPLICIT_LAYER_PATH`, `LSFGVK_CONFIG`, and `LSFGVK_PROFILE`.
- Keeps the original Deck workarounds (WSI off by default, WOW64, vkBasalt, Zink, DXVK cap).

HDR mode and experimental present-mode toggles from the 1.x plugin are omitted: v2 infers HDR from the swapchain format.

## Installation

A ready-to-install zip is in this folder: [`Decky-LSFG-VK-Adaptive.zip`](Decky-LSFG-VK-Adaptive.zip).

1. Copy that zip to the Steam Deck.
2. In Game Mode: Decky → settings cog → enable Developer Mode → Developer → **Install Plugin from Zip**.
3. Open **LSFG Adaptive**, click Install, then add `~/lsfg-vk-adaptive %command%` to the game.

To rebuild the zip after changing the plugin or layer:

```bash
./scripts/package-decky-layer.sh
./scripts/package-decky-plugin.sh
```

## How to use

1. Install [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) from Steam.
2. Open **LSFG Adaptive** in Decky and click **Install**.
3. Enable Adaptive, set Target FPS (for example 90 on OLED), and set Max Multiplier as a ceiling (2–4).
4. Cap the game below the target (in-game limiter or Base FPS Cap for DXVK).
5. Add `~/lsfg-vk-adaptive %command%` to the game's Steam launch options.
6. Restart the game after switching Adaptive on or off.

Do **not** also add `~/lsfg %command%` on the same game. After updating the plugin zip, open LSFG Adaptive and click **Install** again so the layer is moved into the private tree and `~/lsfg-vk-adaptive` is rewritten.

Adaptive uses the same present path as Fixed (game thread). Cap the game below Target FPS so there is room to generate frames. Restart the game after toggling Adaptive.

## License

The plugin sources remain under the original BSD-3-Clause license of decky-lsfg-vk. The lsfg-vk engine in the parent repository is GPL-3.0-or-later.
