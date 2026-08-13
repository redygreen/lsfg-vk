"""
Centralized configuration schema for lsfg-vk.

This module defines the complete configuration structure for lsfg-vk, managing TOML-based config files, including:
- Field definitions with types, defaults, and metadata
- TOML generation logic
- Validation rules
- Type definitions
"""

import logging
import re
import sys
from typing import TypedDict, Dict, Any, Union, cast, List
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

# Import shared configuration constants
sys.path.insert(0, str(Path(__file__).parent.parent.parent))
from shared_config import CONFIG_SCHEMA_DEF, ConfigFieldType, get_field_names, get_defaults, get_field_types

# Import auto-generated configuration components
from .config_schema_generated import ConfigurationData, get_script_parsing_logic, get_script_generation_logic


@dataclass
class ConfigField:
    """Configuration field definition"""
    name: str
    field_type: ConfigFieldType
    default: Union[bool, int, float, str]
    description: str
    
    def get_toml_value(self, value: Union[bool, int, float, str]) -> Union[bool, int, float, str]:
        """Get the value for TOML output"""
        return value


# Use shared configuration schema as source of truth
CONFIG_SCHEMA: Dict[str, ConfigField] = {
    field_name: ConfigField(
        name=field_def["name"],
        field_type=ConfigFieldType(field_def["fieldType"]),
        default=field_def["default"],
        description=field_def["description"]
    )
    for field_name, field_def in CONFIG_SCHEMA_DEF.items()
}

# Override DLL default to empty (will be populated dynamically)
CONFIG_SCHEMA["dll"] = ConfigField(
    name="dll",
    field_type=ConfigFieldType.STRING,
    default="",  # Will be populated dynamically based on detection
    description="specify where Lossless.dll is stored"
)

# Get script-only fields dynamically from shared config
SCRIPT_ONLY_FIELDS = {
    field_name: ConfigField(
        name=field_def["name"],
        field_type=ConfigFieldType(field_def["fieldType"]),
        default=field_def["default"],
        description=field_def["description"]
    )
    for field_name, field_def in CONFIG_SCHEMA_DEF.items()
    if field_def.get("location") == "script"
}

# Complete configuration schema (TOML + script-only fields)
COMPLETE_CONFIG_SCHEMA = {**CONFIG_SCHEMA, **SCRIPT_ONLY_FIELDS}

# Constants for profile management
DEFAULT_PROFILE_NAME = "decky-lsfg-vk"
GLOBAL_SECTION_FIELDS = {"dll", "no_fp16"}
SCRIPT_FIELD_NAMES = {
    field_name
    for field_name, field_def in CONFIG_SCHEMA_DEF.items()
    if field_def.get("location") == "script"
}
TOML_PROFILE_SKIP_FIELDS = GLOBAL_SECTION_FIELDS | SCRIPT_FIELD_NAMES

# Note: ConfigurationData is now imported from generated file
# No need to manually maintain the TypedDict anymore!


class ProfileData(TypedDict):
    """Profile data with current profile tracking"""
    current_profile: str
    profiles: Dict[str, ConfigurationData]  # profile_name -> config
    global_config: Dict[str, Any]  # Global settings (dll, no_fp16)


class ConfigurationManager:
    """Centralized configuration management"""
    
    @staticmethod
    def get_defaults() -> ConfigurationData:
        """Get default configuration values"""
        # Use shared defaults and add script-only fields
        shared_defaults = get_defaults()
        
        # Add script-only fields that aren't in the shared schema
        script_defaults = {
            field.name: field.default 
            for field in SCRIPT_ONLY_FIELDS.values()
        }
        
        return cast(ConfigurationData, {**shared_defaults, **script_defaults})
    
    @staticmethod
    def get_defaults_with_dll_detection(dll_detection_service=None) -> ConfigurationData:
        """Get default configuration values with DLL path detection
        
        Args:
            dll_detection_service: Optional DLL detection service instance
            
        Returns:
            ConfigurationData with detected DLL path if available
        """
        defaults = ConfigurationManager.get_defaults()
        
        # Try to detect DLL path if service provided
        if dll_detection_service:
            try:
                dll_result = dll_detection_service.check_lossless_scaling_dll()
                if dll_result.get("detected") and dll_result.get("path"):
                    defaults["dll"] = dll_result["path"]
            except (OSError, IOError, KeyError, TypeError) as e:
                # If detection fails, keep empty default
                logging.getLogger(__name__).debug(f"DLL detection failed: {e}")
        
        # If detection fails, leave empty so lsfg-vk auto-detects instead of
        # writing a path that does not exist (that aborts layer init).
        return defaults
    
    @staticmethod
    def get_field_names() -> list[str]:
        """Get ordered list of configuration field names"""
        # Use shared field names and add script-only fields
        shared_names = get_field_names()
        script_names = list(SCRIPT_ONLY_FIELDS.keys())
        return shared_names + script_names
    
    @staticmethod
    def get_field_types() -> Dict[str, ConfigFieldType]:
        """Get field type mapping"""
        # Use shared field types and add script-only field types
        shared_types = {name: ConfigFieldType(type_str) for name, type_str in get_field_types().items()}
        script_types = {field.name: field.field_type for field in SCRIPT_ONLY_FIELDS.values()}
        return {**shared_types, **script_types}
    
    @staticmethod
    def validate_config(config: Dict[str, Any]) -> ConfigurationData:
        """Validate and convert configuration data"""
        validated = {}
        
        for field_name, field_def in COMPLETE_CONFIG_SCHEMA.items():
            value = config.get(field_name, field_def.default)
            
            # Type validation and conversion
            if field_def.field_type == ConfigFieldType.BOOLEAN:
                validated[field_name] = bool(value)
            elif field_def.field_type == ConfigFieldType.INTEGER:
                validated[field_name] = int(value)
            elif field_def.field_type == ConfigFieldType.FLOAT:
                validated[field_name] = float(value)
            elif field_def.field_type == ConfigFieldType.STRING:
                validated[field_name] = str(value)
            else:
                validated[field_name] = value

        # lsfg-vk v2 rejects multiplier <= 1
        if int(validated.get("multiplier", 2)) <= 1:
            validated["multiplier"] = 2
        target_fps = float(validated.get("target_fps", 60.0))
        validated["target_fps"] = min(1000.0, max(1.0, target_fps))
        flow_scale = float(validated.get("flow_scale", 0.8))
        validated["flow_scale"] = min(1.0, max(0.25, flow_scale))
        
        return cast(ConfigurationData, validated)
    
    @staticmethod
    def generate_toml_content(config: ConfigurationData) -> str:
        """Generate TOML configuration file content for single profile (backward compatibility)"""
        # For backward compatibility, create a single profile structure
        profile_data: ProfileData = {
            "current_profile": DEFAULT_PROFILE_NAME,
            "profiles": {DEFAULT_PROFILE_NAME: config},
            "global_config": {
                "dll": config.get("dll", ""),
                "no_fp16": config.get("no_fp16", False)
            }
        }
        return ConfigurationManager.generate_toml_content_multi_profile(profile_data)
    
    @staticmethod
    def generate_toml_content_multi_profile(profile_data: ProfileData) -> str:
        """Generate lsfg-vk v2 TOML (version = 2, [[profile]], allow_fp16)."""
        lines = ["version = 2"]
        lines.append("")

        lines.append("[global]")
        lines.append("# Currently selected profile (plugin-only; lsfg-vk ignores this key)")
        lines.append(f'current_profile = "{profile_data["current_profile"]}"')
        lines.append("")

        dll_path = profile_data["global_config"].get("dll", "")
        if dll_path and Path(str(dll_path)).exists():
            lines.append("# specify where Lossless.dll is stored")
            lines.append(f'dll = "{dll_path}"')
        lines.append("")

        lines.append("# FP16 acceleration (inverted from plugin no_fp16)")
        no_fp16 = bool(profile_data["global_config"].get("no_fp16", False))
        lines.append(f"allow_fp16 = {str(not no_fp16).lower()}")
        lines.append("")

        sorted_profiles = sorted(
            profile_data["profiles"].items(),
            key=lambda x: (x[0] != DEFAULT_PROFILE_NAME, x[0]),
        )

        for profile_name, config in sorted_profiles:
            lines.append("[[profile]]")
            if profile_name == DEFAULT_PROFILE_NAME:
                lines.append("# Plugin-managed profile (selected via LSFGVK_PROFILE)")
            else:
                lines.append(f"# Profile: {profile_name}")
            lines.append(f'name = "{profile_name}"')
            lines.append("")

            for field_name, field_def in CONFIG_SCHEMA.items():
                if field_name in TOML_PROFILE_SKIP_FIELDS:
                    continue

                value = config.get(field_name, field_def.default)

                lines.append(f"# {field_def.description}")

                if isinstance(value, bool):
                    lines.append(f"{field_name} = {str(value).lower()}")
                elif isinstance(value, str) and value:
                    lines.append(f'{field_name} = "{value}"')
                elif isinstance(value, float):
                    lines.append(f"{field_name} = {value}")
                elif isinstance(value, int):
                    lines.append(f"{field_name} = {value}")

                lines.append("")

            lines.append("# pacing mode (none = traditional vsync behavior)")
            lines.append('pacing = "none"')
            lines.append("")

        return "\n".join(lines)
    
    @staticmethod
    def parse_toml_content(content: str) -> ConfigurationData:
        """Parse TOML content into configuration data for the currently selected profile (backward compatibility)"""
        profile_data = ConfigurationManager.parse_toml_content_multi_profile(content)
        current_profile = profile_data["current_profile"]
        
        # Merge global config with current profile config
        current_config = profile_data["profiles"].get(current_profile, ConfigurationManager.get_defaults())
        
        # Add global fields to the config
        for field_name in GLOBAL_SECTION_FIELDS:
            if field_name in profile_data["global_config"]:
                current_config[field_name] = profile_data["global_config"][field_name]
        
        return current_config
    
    @staticmethod
    def parse_toml_content_multi_profile(content: str) -> ProfileData:
        """Parse v2 [[profile]] TOML, with fallback for v1 [[game]] files."""
        profiles: Dict[str, ConfigurationData] = {}
        global_config: Dict[str, Any] = {}
        current_profile = DEFAULT_PROFILE_NAME
        
        try:
            lines = content.split('\n')
            in_global_section = False
            in_profile_section = False
            current_profile_name = None
            current_game_config: Dict[str, Any] = {}
            
            def _flush_profile() -> None:
                nonlocal current_profile_name, current_game_config
                if not current_profile_name:
                    return
                validated_config = ConfigurationManager.get_defaults()
                for key, value in current_game_config.items():
                    if key in CONFIG_SCHEMA:
                        field_def = CONFIG_SCHEMA[key]
                        try:
                            if field_def.field_type == ConfigFieldType.BOOLEAN:
                                validated_config[key] = value
                            elif field_def.field_type == ConfigFieldType.INTEGER:
                                validated_config[key] = int(value) if not isinstance(value, int) else value
                            elif field_def.field_type == ConfigFieldType.FLOAT:
                                validated_config[key] = float(value) if not isinstance(value, float) else value
                            elif field_def.field_type == ConfigFieldType.STRING:
                                validated_config[key] = str(value)
                        except (ValueError, TypeError):
                            pass
                profiles[current_profile_name] = validated_config
                current_game_config = {}
            
            for line in lines:
                line = line.strip()
                
                if not line or line.startswith('#'):
                    continue
                
                if line.startswith('[') and line.endswith(']'):
                    if in_profile_section:
                        _flush_profile()
                    
                    if line == '[global]':
                        in_global_section = True
                        in_profile_section = False
                    elif line in ('[[profile]]', '[[game]]'):
                        in_global_section = False
                        in_profile_section = True
                        current_profile_name = None
                    else:
                        in_global_section = False
                        in_profile_section = False
                    continue
                
                if '=' in line:
                    key, value = line.split('=', 1)
                    key = key.strip()
                    value = value.strip()
                    
                    if value.startswith('"') and value.endswith('"'):
                        value = value[1:-1]
                    elif value.startswith("'") and value.endswith("'"):
                        value = value[1:-1]
                    
                    if in_global_section:
                        if key == "current_profile":
                            current_profile = value
                        elif key == "dll":
                            global_config["dll"] = value
                        elif key == "no_fp16":
                            global_config["no_fp16"] = value.lower() in ('true', '1', 'yes', 'on')
                        elif key == "allow_fp16":
                            global_config["no_fp16"] = value.lower() not in ('true', '1', 'yes', 'on')
                    
                    elif in_profile_section:
                        if key in ("name", "exe"):
                            current_profile_name = value
                        elif key in CONFIG_SCHEMA:
                            field_def = CONFIG_SCHEMA[key]
                            try:
                                if field_def.field_type == ConfigFieldType.BOOLEAN:
                                    current_game_config[key] = value.lower() in ('true', '1', 'yes', 'on')
                                elif field_def.field_type == ConfigFieldType.INTEGER:
                                    current_game_config[key] = int(value)
                                elif field_def.field_type == ConfigFieldType.FLOAT:
                                    current_game_config[key] = float(value)
                                elif field_def.field_type == ConfigFieldType.STRING:
                                    current_game_config[key] = value
                            except (ValueError, TypeError):
                                pass
            
            if in_profile_section:
                _flush_profile()
            
            if not profiles:
                profiles[DEFAULT_PROFILE_NAME] = ConfigurationManager.get_defaults()
            
            if current_profile not in profiles:
                current_profile = DEFAULT_PROFILE_NAME
                if DEFAULT_PROFILE_NAME not in profiles:
                    profiles[DEFAULT_PROFILE_NAME] = ConfigurationManager.get_defaults()
            
            return ProfileData(
                current_profile=current_profile,
                profiles=profiles,
                global_config=global_config
            )
            
        except (ValueError, KeyError, TypeError, AttributeError) as e:
            logging.getLogger(__name__).warning(f"Failed to parse TOML profiles, using defaults: {e}")
            return ProfileData(
                current_profile=DEFAULT_PROFILE_NAME,
                profiles={DEFAULT_PROFILE_NAME: ConfigurationManager.get_defaults()},
                global_config={}
            )
    
    @staticmethod
    def parse_script_content(script_content: str) -> Dict[str, Union[bool, int, str]]:
        """Parse launch script content to extract environment variable values
        
        Args:
            script_content: Content of the launch script file
            
        Returns:
            Dict containing parsed script-only field values
        """
        # Use auto-generated parsing logic
        parse_script_values = get_script_parsing_logic()
        return parse_script_values(script_content.split('\n'))
    
    @staticmethod
    def merge_config_with_script(toml_config: ConfigurationData, script_values: Dict[str, Union[bool, int, str]]) -> ConfigurationData:
        """Merge TOML configuration with script environment variable values
        
        Args:
            toml_config: Configuration loaded from TOML file
            script_values: Environment variable values parsed from script
            
        Returns:
            Complete configuration with script values overlaid on TOML config
        """
        merged_config = dict(toml_config)
        
        # Update script-only fields with values from script
        for field_name in SCRIPT_ONLY_FIELDS.keys():
            if field_name in script_values:
                merged_config[field_name] = script_values[field_name]
        
        return cast(ConfigurationData, merged_config)

    @staticmethod
    def normalize_profile_name(profile_name: str) -> str:
        """Normalize profile name by converting spaces to dashes and trimming
        
        This allows users to enter names with spaces, which are then safely
        converted to dashes for storage and shell script compatibility.
        
        Args:
            profile_name: The raw profile name from user input
            
        Returns:
            Normalized profile name with spaces converted to dashes
        """
        if not profile_name:
            return profile_name
        
        # Trim whitespace and convert spaces to dashes
        normalized = profile_name.strip().replace(' ', '-')
        
        # Collapse multiple consecutive dashes into one
        while '--' in normalized:
            normalized = normalized.replace('--', '-')
        
        # Remove leading/trailing dashes
        normalized = normalized.strip('-')
        
        return normalized
    
    @staticmethod
    def validate_profile_name(profile_name: str) -> bool:
        """Validate profile name for safety (after normalization)"""
        if not profile_name:
            return False
        
        # Normalize first - this converts spaces to dashes
        normalized = ConfigurationManager.normalize_profile_name(profile_name)
        
        if not normalized:
            return False
        
        # Check for invalid characters that could cause issues in shell scripts or TOML
        # Note: spaces are now allowed as input (they get converted to dashes)
        invalid_chars = set('\t\n\r\'"\\/$|&;()<>{}[]`*?')
        if any(char in invalid_chars for char in normalized):
            return False
        
        # Check for reserved names
        reserved_names = {'global', 'game', 'current_profile'}
        if normalized.lower() in reserved_names:
            return False
        
        return True
    
    @staticmethod
    def create_profile(profile_data: ProfileData, profile_name: str, source_profile: str = None) -> ProfileData:
        """Create a new profile by copying from source profile or defaults"""
        if not ConfigurationManager.validate_profile_name(profile_name):
            raise ValueError(f"Invalid profile name: {profile_name}")
        
        # Normalize the profile name (converts spaces to dashes)
        profile_name = ConfigurationManager.normalize_profile_name(profile_name)
        
        if profile_name in profile_data["profiles"]:
            raise ValueError(f"Profile '{profile_name}' already exists")
        
        # Copy from source profile or use defaults
        if source_profile and source_profile in profile_data["profiles"]:
            new_config = dict(profile_data["profiles"][source_profile])
        else:
            new_config = ConfigurationManager.get_defaults()
        
        # Create new profile data structure
        new_profile_data = ProfileData(
            current_profile=profile_data["current_profile"],
            profiles=dict(profile_data["profiles"]),
            global_config=dict(profile_data["global_config"])
        )
        new_profile_data["profiles"][profile_name] = new_config
        
        return new_profile_data
    
    @staticmethod
    def delete_profile(profile_data: ProfileData, profile_name: str) -> ProfileData:
        """Delete a profile (cannot delete default profile)"""
        if profile_name == DEFAULT_PROFILE_NAME:
            raise ValueError(f"Cannot delete default profile '{DEFAULT_PROFILE_NAME}'")
        
        if profile_name not in profile_data["profiles"]:
            raise ValueError(f"Profile '{profile_name}' does not exist")
        
        # Create new profile data structure
        new_profile_data = ProfileData(
            current_profile=profile_data["current_profile"],
            profiles=dict(profile_data["profiles"]),
            global_config=dict(profile_data["global_config"])
        )
        
        # Remove the profile
        del new_profile_data["profiles"][profile_name]
        
        # If we deleted the current profile, switch to default
        if new_profile_data["current_profile"] == profile_name:
            new_profile_data["current_profile"] = DEFAULT_PROFILE_NAME
            # Ensure default profile exists
            if DEFAULT_PROFILE_NAME not in new_profile_data["profiles"]:
                new_profile_data["profiles"][DEFAULT_PROFILE_NAME] = ConfigurationManager.get_defaults()
        
        return new_profile_data
    
    @staticmethod
    def rename_profile(profile_data: ProfileData, old_name: str, new_name: str) -> ProfileData:
        """Rename a profile"""
        if old_name == DEFAULT_PROFILE_NAME:
            raise ValueError(f"Cannot rename default profile '{DEFAULT_PROFILE_NAME}'")
        
        if not ConfigurationManager.validate_profile_name(new_name):
            raise ValueError(f"Invalid profile name: {new_name}")
        
        # Normalize the new name (converts spaces to dashes)
        new_name = ConfigurationManager.normalize_profile_name(new_name)
        
        if old_name not in profile_data["profiles"]:
            raise ValueError(f"Profile '{old_name}' does not exist")
        
        if new_name in profile_data["profiles"]:
            raise ValueError(f"Profile '{new_name}' already exists")
        
        # Create new profile data structure
        new_profile_data = ProfileData(
            current_profile=profile_data["current_profile"],
            profiles={},
            global_config=dict(profile_data["global_config"])
        )
        
        # Copy profiles with new name
        for profile_name, config in profile_data["profiles"].items():
            if profile_name == old_name:
                new_profile_data["profiles"][new_name] = dict(config)
            else:
                new_profile_data["profiles"][profile_name] = dict(config)
        
        # Update current_profile if necessary
        if new_profile_data["current_profile"] == old_name:
            new_profile_data["current_profile"] = new_name
        
        return new_profile_data
    
    @staticmethod
    def set_current_profile(profile_data: ProfileData, profile_name: str) -> ProfileData:
        """Set the current active profile"""
        if profile_name not in profile_data["profiles"]:
            raise ValueError(f"Profile '{profile_name}' does not exist")
        
        # Create new profile data structure
        new_profile_data = ProfileData(
            current_profile=profile_name,
            profiles=dict(profile_data["profiles"]),
            global_config=dict(profile_data["global_config"])
        )
        
        return new_profile_data
