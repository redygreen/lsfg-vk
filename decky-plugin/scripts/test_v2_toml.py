#!/usr/bin/env python3
"""Sanity-check v2 TOML generate/parse used by the Adaptive Decky plugin."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "py_modules"))

from lsfg_vk.config_schema import ConfigurationManager, DEFAULT_PROFILE_NAME


def main() -> None:
    defaults = ConfigurationManager.get_defaults()
    defaults["dll"] = "/tmp/Lossless.dll"
    defaults["adaptive"] = True
    defaults["target_fps"] = 90.0
    defaults["multiplier"] = 4
    defaults["no_fp16"] = False

    toml = ConfigurationManager.generate_toml_content(defaults)
    assert "version = 2" in toml, toml
    assert "[[profile]]" in toml, toml
    assert "[[game]]" not in toml, toml
    assert "allow_fp16 = true" in toml, toml
    assert "no_fp16 =" not in toml, toml
    assert "adaptive = true" in toml, toml
    assert "target_fps = 90" in toml, toml
    assert f'name = "{DEFAULT_PROFILE_NAME}"' in toml, toml
    assert "hdr_mode" not in toml, toml
    assert "experimental_present_mode" not in toml, toml
    assert "dxvk_frame_rate" not in toml, toml

    parsed = ConfigurationManager.parse_toml_content(toml)
    assert parsed["adaptive"] is True
    assert parsed["target_fps"] == 90.0
    assert parsed["multiplier"] == 4
    assert parsed["no_fp16"] is False
    assert parsed["dll"] == "/tmp/Lossless.dll"

    clamped = ConfigurationManager.validate_config({"multiplier": 1, "target_fps": 0})
    assert clamped["multiplier"] == 2
    assert clamped["target_fps"] == 1.0

    print("v2 TOML generate/parse OK")
    print(toml)


if __name__ == "__main__":
    main()
