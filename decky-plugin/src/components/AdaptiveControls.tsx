import { PanelSectionRow, ToggleField, SliderField } from "@decky/ui";
import { ConfigurationData } from "../config/configSchema";
import { ADAPTIVE, TARGET_FPS } from "../config/generatedConfigSchema";
import t from '../i18n/i18n';

interface AdaptiveControlsProps {
  config: ConfigurationData;
  onConfigChange: (fieldName: keyof ConfigurationData, value: boolean | number | string) => Promise<void>;
}

export function AdaptiveControls({
  config,
  onConfigChange
}: AdaptiveControlsProps) {
  return (
    <>
      <PanelSectionRow>
        <ToggleField
          label={t('CONFIG_ADAPTIVE', 'Adaptive Frame Generation')}
          description={t('CONFIG_ADAPTIVE_DESC', 'Generate extra frames during present to approach Target FPS. Multiplier is a ceiling. Use Off on the multiplier switch to disable FG without restarting.')}
          checked={config.adaptive}
          onChange={(value) => onConfigChange(ADAPTIVE, value)}
        />
      </PanelSectionRow>

      {config.adaptive && (
        <PanelSectionRow>
          <SliderField
            label={`${t('CONFIG_TARGET_FPS', 'Target FPS')} (${Math.round(config.target_fps)})`}
            description={t('CONFIG_TARGET_FPS_DESC', 'Desired displayed framerate. Cap the game below this so Adaptive has room to generate frames.')}
            value={config.target_fps}
            min={30}
            max={120}
            step={1}
            onChange={(value) => onConfigChange(TARGET_FPS, value)}
          />
        </PanelSectionRow>
      )}
    </>
  );
}
