"""
Constants for the lsfg-vk Adaptive Decky plugin.

The Vulkan layer is installed into a plugin-private tree, matching
eugeniosegala/decky-lsfg-vk-experimental. That keeps it off the default
implicit-layer search path so official decky-lsfg-vk cannot load
alongside this fork. The launch wrapper sets VK_IMPLICIT_LAYER_PATH to
the private manifest directory for games that use ~/lsfg-vk-adaptive.
"""

from pathlib import Path

# Private install root. Do not use ~/.local/share/vulkan/implicit_layer.d:
# that is the global implicit-layer path shared with official lsfg-vk.
ADAPTIVE_ROOT = ".local/share/decky-lsfg-vk-adaptive"
LOCAL_LIB = f"{ADAPTIVE_ROOT}/lib"
VULKAN_LAYER_DIR = f"{ADAPTIVE_ROOT}/vulkan/implicit_layer.d"
CONFIG_DIR = ".config/lsfg-vk-adaptive"

SCRIPT_NAME = "lsfg-vk-adaptive"
CONFIG_FILENAME = "conf.toml"
LOG_FILENAME = "lsfg-vk.log"
LAYER_LOG_FILENAME = "lsfg-vk-layer.log"
STATS_FILENAME = "stats.json"
LIB_FILENAME = "liblsfg-vk-layer.so"
JSON_FILENAME = "VkLayer_LSFGVK_frame_generation.json"
# Manifest lives at <root>/vulkan/implicit_layer.d, library at <root>/lib.
LAYER_LIBRARY_RELATIVE_PATH = f"../../lib/{LIB_FILENAME}"

# Earlier Adaptive installs dropped files into the global implicit-layer
# path. Remove them on install/uninstall so they cannot keep loading.
LEGACY_LIB = f".local/lib/{LIB_FILENAME}"
LEGACY_JSON = f".local/share/vulkan/implicit_layer.d/{JSON_FILENAME}"
ZIP_FILENAME = "lsfg-vk_noui.zip"
ARM_LIB_FILENAME = "liblsfg-vk-arm64.so"

FLATPAK_23_08_FILENAME = "org.freedesktop.Platform.VulkanLayer.lsfg_vk_23.08.flatpak"
FLATPAK_24_08_FILENAME = "org.freedesktop.Platform.VulkanLayer.lsfg_vk_24.08.flatpak"
FLATPAK_25_08_FILENAME = "org.freedesktop.Platform.VulkanLayer.lsfg_vk_25.08.flatpak"

SO_EXT = ".so"
JSON_EXT = ".json"

BIN_DIR = "bin"

ARMADA_DEVICE_ENV = Path("/usr/libexec/armada/device-env")
ARMADA_GAME_LAUNCH = Path("/usr/libexec/armada/armada-game-launch")

STEAM_COMMON_PATH = Path("steamapps/common/Lossless Scaling")
LOSSLESS_DLL_NAME = "Lossless.dll"

ENV_LSFG_DLL_PATH = "LSFG_DLL_PATH"
ENV_XDG_DATA_HOME = "XDG_DATA_HOME"
ENV_HOME = "HOME"

LAUNCH_OPTION = "~/lsfg-vk-adaptive %command%"
