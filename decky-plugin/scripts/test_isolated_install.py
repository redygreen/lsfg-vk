#!/usr/bin/env python3
"""Verify Adaptive Decky install stays off the official lsfg-vk Vulkan path."""

import json
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "py_modules"))

sys.modules.setdefault(
    "decky",
    SimpleNamespace(
        logger=SimpleNamespace(
            info=lambda *a, **k: None,
            warning=lambda *a, **k: None,
            error=lambda *a, **k: None,
            debug=lambda *a, **k: None,
        )
    ),
)

from lsfg_vk.config_schema import ConfigurationManager, DEFAULT_PROFILE_NAME
from lsfg_vk.configuration import ConfigurationService
from lsfg_vk.constants import (
    ADAPTIVE_ROOT,
    JSON_FILENAME,
    LAYER_LIBRARY_RELATIVE_PATH,
    LEGACY_JSON,
    LEGACY_LIB,
    LIB_FILENAME,
    STATS_FILENAME,
    VULKAN_LAYER_DIR,
)
from lsfg_vk.installation import InstallationService


class _Logger:
    def info(self, *args, **kwargs):
        pass

    def warning(self, *args, **kwargs):
        pass

    def error(self, *args, **kwargs):
        pass

    def debug(self, *args, **kwargs):
        pass


def test_constants_are_private():
    assert ADAPTIVE_ROOT == ".local/share/decky-lsfg-vk-adaptive"
    assert "vulkan/implicit_layer.d" not in ADAPTIVE_ROOT
    assert VULKAN_LAYER_DIR.startswith(ADAPTIVE_ROOT)
    assert VULKAN_LAYER_DIR != ".local/share/vulkan/implicit_layer.d"
    assert LEGACY_JSON == ".local/share/vulkan/implicit_layer.d/" + JSON_FILENAME
    assert LEGACY_LIB == ".local/lib/" + LIB_FILENAME
    assert LAYER_LIBRARY_RELATIVE_PATH == "../../lib/" + LIB_FILENAME
    assert STATS_FILENAME == "stats.json"


def test_wrapper_sets_isolated_layer_path():
    with tempfile.TemporaryDirectory() as tmp:
        home = Path(tmp)
        with patch("pathlib.Path.home", return_value=home):
            service = ConfigurationService(logger=_Logger())

        script = service._generate_script_content(ConfigurationManager.get_defaults())
        private_json_dir = home / VULKAN_LAYER_DIR
        assert "export DISABLE_LSFG=1" in script
        assert f"export VK_IMPLICIT_LAYER_PATH={private_json_dir}" in script
        assert str(home / ".local/share/vulkan/implicit_layer.d") not in script
        assert "export LSFGVK_CONFIG=" in script
        assert "lsfg-vk-adaptive" in str(service.config_file_path)
        assert f"export LSFGVK_PROFILE={DEFAULT_PROFILE_NAME}" in script
        assert "lsfg-vk.log" in script
        assert "lsfg-vk-layer.log" in script
        assert "export LSFGVK_LOG=" in script
        assert "argc=$#" in script


def test_json_library_path_matches_private_tree():
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "in.json"
        dst = Path(tmp) / "out.json"
        src.write_text(json.dumps({
            "file_format_version": "1.1.0",
            "layer": {
                "name": "VK_LAYER_LSFGVK_frame_generation",
                "library_path": LIB_FILENAME,
            },
        }))
        with patch("pathlib.Path.home", return_value=Path(tmp) / "home"):
            installer = InstallationService(logger=_Logger())
        installer._copy_and_fix_json_file(src, dst)
        rewritten = json.loads(dst.read_text())
        assert rewritten["layer"]["library_path"] == LAYER_LIBRARY_RELATIVE_PATH


def test_legacy_global_files_are_removed_without_touching_official():
    with tempfile.TemporaryDirectory() as tmp:
        home = Path(tmp)
        official_lib = home / ".local/lib/liblsfg-vk.so"
        official_json = home / ".local/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation.json"
        leftover_lib = home / LEGACY_LIB
        leftover_json = home / LEGACY_JSON
        for path in (official_lib, official_json, leftover_lib, leftover_json):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("keep-or-drop")

        with patch("pathlib.Path.home", return_value=home):
            installer = InstallationService(logger=_Logger())
        removed = installer._remove_legacy_global_layer_files()

        assert str(leftover_lib) in removed
        assert str(leftover_json) in removed
        assert not leftover_lib.exists()
        assert not leftover_json.exists()
        assert official_lib.exists()
        assert official_json.exists()


def test_adaptive_stats_reader():
    import asyncio
    from lsfg_vk.plugin import Plugin

    with tempfile.TemporaryDirectory() as tmp:
        home = Path(tmp)
        stats_dir = home / ".config" / "lsfg-vk-adaptive"
        stats_dir.mkdir(parents=True)
        (stats_dir / STATS_FILENAME).write_text(
            '{"real_fps":45.2,"generated_fps":44.1,"displayed_fps":89.3,'
            '"avg_gen":0.98,"target_fps":90.0,"adaptive":true}\n',
            encoding="utf-8",
        )
        with patch("lsfg_vk.plugin.Path.home", return_value=home):
            plugin = Plugin()
            result = asyncio.run(plugin.get_adaptive_stats())
        assert result["success"] is True
        assert result["stale"] is False
        assert result["real_fps"] == 45.2
        assert result["generated_fps"] == 44.1
        assert result["displayed_fps"] == 89.3


def main() -> None:
    test_constants_are_private()
    test_wrapper_sets_isolated_layer_path()
    test_json_library_path_matches_private_tree()
    test_legacy_global_files_are_removed_without_touching_official()
    test_adaptive_stats_reader()
    print("isolated install paths OK")


if __name__ == "__main__":
    main()
