import { useEffect, useState } from "react";
import { PanelSectionRow, ToggleField } from "@decky/ui";
import { getAdaptiveStats, type AdaptiveStatsResult } from "../api/lsfgApi";
import t from "../i18n/i18n";

const SHOW_STATS_KEY = "lsfg-adaptive-show-stats";

function readShowStats(): boolean {
  try {
    const saved = localStorage.getItem(SHOW_STATS_KEY);
    return saved !== null ? JSON.parse(saved) : true;
  } catch {
    return true;
  }
}

function StatRow({ label, value, dim }: { label: string; value: string; dim?: boolean }) {
  return (
    <div
      style={{
        display: "flex",
        justifyContent: "space-between",
        alignItems: "baseline",
        marginBottom: "4px",
        opacity: dim ? 0.55 : 1
      }}
    >
      <span style={{ fontSize: "13px", color: "rgba(255,255,255,0.75)" }}>{label}</span>
      <span style={{ fontSize: "22px", fontWeight: 700, fontVariantNumeric: "tabular-nums" }}>
        {value}
      </span>
    </div>
  );
}

export function AdaptiveStats() {
  const [show, setShow] = useState(readShowStats);
  const [stats, setStats] = useState<AdaptiveStatsResult | null>(null);

  useEffect(() => {
    try {
      localStorage.setItem(SHOW_STATS_KEY, JSON.stringify(show));
    } catch {
      /* ignore quota / private mode */
    }
  }, [show]);

  useEffect(() => {
    if (!show)
      return undefined;

    let cancelled = false;
    const tick = async () => {
      try {
        const next = await getAdaptiveStats();
        if (!cancelled)
          setStats(next);
      } catch {
        if (!cancelled)
          setStats({ success: false, stale: true, real_fps: 0, generated_fps: 0, displayed_fps: 0, avg_gen: 0, target_fps: 0, adaptive: false });
      }
    };

    tick();
    const id = window.setInterval(tick, 1000);
    return () => {
      cancelled = true;
      window.clearInterval(id);
    };
  }, [show]);

  const stale = !stats || stats.stale || !stats.success;
  const fmt = (n: number) => (stale ? "—" : n.toFixed(0));

  return (
    <>
      <PanelSectionRow>
        <ToggleField
          label={t("CONFIG_SHOW_STATS", "Show FPS stats")}
          description={t(
            "CONFIG_SHOW_STATS_DESC",
            "Live real / generated / displayed FPS. Open this menu while a game is running. Cannot draw over the game on Deck."
          )}
          checked={show}
          onChange={setShow}
        />
      </PanelSectionRow>

      {show && (
        <PanelSectionRow>
          <div
            style={{
              width: "100%",
              padding: "10px 12px",
              marginBottom: "8px",
              borderRadius: "8px",
              background: "rgba(255,255,255,0.06)"
            }}
          >
            <StatRow
              label={t("STATS_REAL", "Real FPS")}
              value={fmt(stats?.real_fps ?? 0)}
              dim={stale}
            />
            <StatRow
              label={t("STATS_GENERATED", "Generated FPS")}
              value={fmt(stats?.generated_fps ?? 0)}
              dim={stale}
            />
            <StatRow
              label={t("STATS_DISPLAYED", "On screen")}
              value={fmt(stats?.displayed_fps ?? 0)}
              dim={stale}
            />
            <div
              style={{
                marginTop: "6px",
                fontSize: "12px",
                color: "rgba(255,255,255,0.55)"
              }}
            >
              {stale
                ? t("STATS_IDLE", "No game reporting — launch with ~/lsfg-vk-adaptive %command%")
                : t("STATS_LIVE", "Updated every second from the Vulkan layer")}
            </div>
          </div>
        </PanelSectionRow>
      )}
    </>
  );
}
