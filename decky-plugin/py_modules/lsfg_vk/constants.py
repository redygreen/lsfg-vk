"""
Constants for the lsfg-vk Adaptive Decky plugin.

Paths are isolated from the official 1.x decky-lsfg-vk plugin so both can
be installed at once: different layer filenames, config dir, and wrapper.
"""

from pathlib import Path

LOCAL_LIB = ".local/lib"
LOCAL_SHARE_BASE = ".local/share"
VULKAN_LAYER_DIR = ".local/share/vulkan/implicit_layer.d"
CONFIG_DIR = ".config/lsfg-vk-adaptive"

SCRIPT_NAME = "lsfg-vk-adaptive"
CONFIG_FILENAME = "conf.toml"
LIB_FILENAME = "liblsfg-vk-layer.so"
JSON_FILENAME = "VkLayer_LSFGVK_frame_generation.json"
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
