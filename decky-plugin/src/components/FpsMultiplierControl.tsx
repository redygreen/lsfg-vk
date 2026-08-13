import { PanelSectionRow, DialogButton, Focusable } from "@decky/ui";
import { ConfigurationData } from "../config/configSchema";
import t from '../i18n/i18n';

interface FpsMultiplierControlProps {
  config: ConfigurationData;
  onConfigPatch: (patch: Partial<ConfigurationData>) => Promise<void>;
}

export function FpsMultiplierControl({
  config,
  onConfigPatch
}: FpsMultiplierControlProps) {
  const fgOn = config.enabled !== false;
  const label = !fgOn
    ? t('MULTIPLIER_OFF', 'Off')
    : config.adaptive
      ? `${t('MULTIPLIER_MAX', 'Max')} ${config.multiplier}X`
      : `${config.multiplier}X`;

  const minus = () => {
    if (!fgOn)
      return;
    if (config.multiplier <= 2) {
      void onConfigPatch({ enabled: false, multiplier: 2 });
      return;
    }
    void onConfigPatch({ enabled: true, multiplier: config.multiplier - 1 });
  };

  const plus = () => {
    if (!fgOn) {
      void onConfigPatch({ enabled: true, multiplier: 2 });
      return;
    }
    if (config.multiplier >= 4)
      return;
    void onConfigPatch({ enabled: true, multiplier: Math.min(4, config.multiplier + 1) });
  };

  return (
    <PanelSectionRow>
      <Focusable
        style={{
          marginTop: "6px",
          marginBottom: "6px",
          display: "flex",
          justifyContent: "center",
          alignItems: "center"
        }}
        flow-children="horizontal"
      >
        <DialogButton
          style={{
            marginLeft: "0px",
            height: "30px",
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            padding: "5px 0px 0px 0px",
            minWidth: "40px",
          }}
          onClick={minus}
          disabled={!fgOn}
        >
          −
        </DialogButton>
        <div
          style={{
            marginLeft: "20px",
            marginRight: "20px",
            fontSize: "16px",
            fontWeight: "bold",
            color: fgOn && config.multiplier > 4 ? "red" : "white",
            minWidth: "80px",
            textAlign: "center"
          }}
        >
          {label}
        </div>
        <DialogButton
          style={{
            marginLeft: "0px",
            height: "30px",
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            padding: "5px 0px 0px 0px",
            minWidth: "40px",
          }}
          onClick={plus}
          disabled={fgOn && config.multiplier >= 4}
        >
          +
        </DialogButton>
      </Focusable>
    </PanelSectionRow>
  );
}
