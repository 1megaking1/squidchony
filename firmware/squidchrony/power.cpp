#include "power.h"

void PowerPolicy::begin(uint32_t now_ms, bool power_button_boot, const PowerConfig& cfg) {
  cfg_ = cfg;
  boot_ms_ = now_ms;
  boot_check_ = power_button_boot;
  boot_press_ = power_button_boot;
  off_requested_ = false;
  last_activity_ms_ = now_ms;
  last_needed_ms_ = now_ms;  // switching on counts as a use: the radar is ready soon after
  radar_on_ = false;
  settled_ = false;
  low_ = false;
}

PowerOutputs PowerPolicy::update(const PowerInputs& in) {
  PowerOutputs out;
  const uint32_t now = in.now_ms;

  // A power-button boot only sticks if MODE is still held a moment later; a brief knock in a
  // bag switches straight back off. The radar stays off until then.
  if (boot_check_) {
    if (!in.mode_held) {
      radar_on_ = false;
      out.off = OffReason::BootTap;
      return out;
    }
    if (now - boot_ms_ >= cfg_.boot_hold_ms) {
      boot_check_ = false;
      last_activity_ms_ = now;
      last_needed_ms_ = now;
    }
  }

  if (in.activity) last_activity_ms_ = now;
  if (in.want_measure) last_needed_ms_ = now;

  // Radar: on while measuring is wanted and for a while after, so repeated presses of READ
  // don't each wait for it to settle.
  bool want_radar = !boot_check_ && (cfg_.radar_hold_ms == 0 || in.want_measure ||
                                     now - last_needed_ms_ < cfg_.radar_hold_ms);
  if (want_radar && !radar_on_) {
    radar_on_ = true;
    radar_on_ms_ = now;
    settled_ = false;
  } else if (!want_radar && radar_on_) {
    radar_on_ = false;
    settled_ = false;
  }
  if (radar_on_ && !settled_ && now - radar_on_ms_ >= cfg_.radar_settle_ms) {
    settled_ = true;
    out.radar_settled = true;
  }
  out.booting = boot_check_;
  out.radar_on = radar_on_;
  out.measuring = radar_on_ && settled_ && in.want_measure;

  // Battery: switch off before the protection board has to, so the shot log gets saved.
  if (in.batt_valid && in.batt_v > 0.5f && in.batt_v < cfg_.low_batt_v) {
    if (!low_) {
      low_ = true;
      low_since_ms_ = now;
    }
    if (now - low_since_ms_ >= cfg_.low_batt_ms) out.off = OffReason::Battery;
  } else if (!in.batt_valid || in.batt_v >= cfg_.low_batt_v + 0.1f) {
    low_ = false;
  }

  // Idle: warn, then switch off.
  if (cfg_.auto_off_ms) {
    uint32_t idle = now - last_activity_ms_;
    if (idle >= cfg_.auto_off_ms) {
      if (out.off == OffReason::None) out.off = OffReason::Idle;
    } else if (idle + cfg_.auto_off_warn_ms >= cfg_.auto_off_ms) {
      out.off_in_ms = cfg_.auto_off_ms - idle;
    }
  }

  if (boot_press_) {
    if (!in.mode_held)
      boot_press_ = false;
    else if (now - boot_ms_ >= cfg_.stuck_ms && out.off == OffReason::None)
      out.off = OffReason::Stuck;
  }

  if (off_requested_) out.off = OffReason::User;
  if (out.off != OffReason::None) out.measuring = false;
  return out;
}

const char* offReasonText(OffReason r) {
  switch (r) {
    case OffReason::User: return "Saving your shots";
    case OffReason::Idle: return "Not used for a while";
    case OffReason::Battery: return "Battery empty: charge it";
    case OffReason::Stuck: return "MODE is held down";
    case OffReason::BootTap: return "";
    default: return "";
  }
}
