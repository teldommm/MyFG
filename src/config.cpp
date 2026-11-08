#include "config.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

Config g_cfg;

void Config::parse(const char* blob) {
    if (!blob) return;
    std::string s(blob);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        std::string line = trim(s.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        int v = atoi(trim(line.substr(eq + 1)).c_str());
#define KEY(name) if (k == #name) { name = v; continue; }
        KEY(enable) KEY(min_fps) KEY(max_dt_ms) KEY(acq_fence) KEY(hook_egl) KEY(hook_vk)
        KEY(hook_surface) KEY(hook_delay_ms) KEY(hook_anw) KEY(zorder) KEY(display_fps) KEY(release_lag) KEY(stats)
        KEY(target_w) KEY(target_h) KEY(max_gen) KEY(target_fps) KEY(flow_side)
        KEY(auto_rate) KEY(dataspace) KEY(hide_ms)
        KEY(hook_swap) KEY(hook_present) KEY(fallback_ms) KEY(panel_hz) KEY(panel_game_vote) KEY(panel_anchor)
        KEY(multiplier) KEY(dup_detect) KEY(dup_thresh) KEY(use_oncomplete) KEY(queue_prio) KEY(elect_blast)
        KEY(layer_tf) KEY(hook_bbq) KEY(ui_source) KEY(hook_sc) KEY(live) KEY(debug) KEY(hud) KEY(hud_mode) KEY(hud_pos) KEY(hud_scale) KEY(hud_tf) KEY(hdr) KEY(hdr_pw) KEY(hdr_peak) KEY(hdr_knee) KEY(hdr_boost) KEY(hdr_gamma) KEY(hdr_ct) KEY(hdr_sat) KEY(adaptive) KEY(gpu_budget)
        KEY(thermal) KEY(thermal_hi) KEY(thermal_lo) KEY(thermal_hz)
        KEY(pts_phase) KEY(qcom_me) KEY(me_refine) KEY(me_dy_sign)
        KEY(pacing)
#undef KEY
    }
    sanitize();
}

void Config::sanitize() {
    auto clampi = [](int& v, int lo, int hi) { if (v < lo) v = lo; else if (v > hi) v = hi; };
    clampi(enable, 0, 1);
    clampi(min_fps, 0, 240);
    clampi(max_dt_ms, 50, 2000);
    clampi(acq_fence, 0, 1);
    clampi(hook_surface, 0, 1);
    clampi(hook_egl, 0, 2);
    clampi(hook_anw, 0, 2);
    clampi(hook_vk, 0, 1);
    clampi(hook_delay_ms, 0, 60000);
    clampi(zorder, -8, 8);
    clampi(display_fps, 0, 480);
    clampi(auto_rate, 0, 1);
    clampi(dataspace, 0, 1);
    clampi(hide_ms, 0, 10000);
    clampi(release_lag, 0, 8);
    clampi(stats, 0, 1);
    clampi(target_w, 0, 16384);
    clampi(target_h, 0, 16384);
    clampi(max_gen, 0, 7);
    clampi(target_fps, 0, 480);
    clampi(flow_side, 64, 1080);
    clampi(hook_swap, 0, 2);
    clampi(hook_present, 0, 2);
    clampi(fallback_ms, 1000, 120000);
    clampi(panel_hz, 0, 480);
    clampi(panel_game_vote, 0, 1);
    clampi(panel_anchor, 0, 1);
    if (multiplier == 1) multiplier = 0;
    clampi(multiplier, 0, 8);
    clampi(dup_detect, 0, 1);
    clampi(dup_thresh, 0, 64);
    clampi(use_oncomplete, 0, 1);
    clampi(queue_prio, 0, 1);
    clampi(elect_blast, 0, 1);
    clampi(hook_bbq, 0, 1);
    clampi(hook_sc, -1, 1);
    clampi(ui_source, 0, 1);
    clampi(layer_tf, -1, 7);
    clampi(live, 0, 1);
    clampi(debug, 0, 1);
    clampi(hud, 0, 1);
    clampi(hud_mode, 0, 2);
    clampi(hud_pos, 0, 3);
    clampi(hud_scale, 1, 8);
    clampi(hud_tf, 0, 7);
    clampi(hdr, 0, 1);
    clampi(hdr_pw, 100, 1000);
    if (hdr_peak != 0) clampi(hdr_peak, 400, 1400);
    clampi(hdr_knee, 10, 100);
    clampi(hdr_boost, 0, 100);
    if (hdr_gamma != 0) clampi(hdr_gamma, 18, 26);
    clampi(hdr_ct, 50, 150);
    clampi(hdr_sat, 50, 150);
    clampi(adaptive, 0, 1);
    clampi(gpu_budget, 10, 90);
    clampi(thermal, 0, 1);
    clampi(thermal_hi, 35, 60);
    clampi(thermal_lo, 25, 59);
    if (thermal_lo >= thermal_hi) thermal_lo = thermal_hi - 1;   // гистерезис не вырождается в ноль
    clampi(thermal_hz, 30, 144);
    clampi(pts_phase, 0, 1);
    clampi(qcom_me, 0, 1);
    clampi(me_refine, 0, 2);
    clampi(me_dy_sign, 0, 2);
    clampi(pacing, 0, 1);
    if (target_w == 0 || target_h == 0) { target_w = 0; target_h = 0; }   // только пара — иначе размер не задан
}

// Поля, которые читаются на каждом кадре и поэтому безопасно меняются в работе. Хуки, выбор очереди
// и параметры создания устройства сюда не входят — они применяются только при старте игры.
void Config::applyLive(const Config& n) {
#define L(f) f = n.f;
    L(enable) L(min_fps) L(max_dt_ms) L(acq_fence) L(zorder) L(display_fps) L(auto_rate) L(dataspace) L(hide_ms)
    L(release_lag) L(stats) L(max_gen) L(target_fps) L(flow_side) L(panel_hz) L(panel_game_vote) L(panel_anchor)
    L(multiplier) L(dup_detect) L(dup_thresh) L(use_oncomplete) L(layer_tf) L(hdr) L(hdr_pw) L(hdr_peak) L(hdr_knee) L(hdr_boost) L(hdr_gamma) L(hdr_ct) L(hdr_sat) L(adaptive) L(gpu_budget)
    L(thermal) L(thermal_hi) L(thermal_lo) L(thermal_hz) L(debug) L(hud) L(hud_mode) L(hud_pos) L(hud_scale) L(hud_tf)
    L(pts_phase) L(qcom_me) L(me_refine) L(me_dy_sign) L(pacing)
    // Читаются на каждом кадре (elect()/blast_hint()), поэтому безопасно меняются на ходу.
    // Прежде live-правка target_w/target_h/elect_blast молча не действовала до рестарта игры.
    L(target_w) L(target_h) L(elect_blast) L(ui_source)
#undef L
}
