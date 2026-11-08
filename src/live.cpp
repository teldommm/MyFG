#include "live.h"
#include "common.h"
#include "config.h"

#include <fcntl.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace live {

void start(const char* package, void (*on_applied)()) {
    if (!package || !*package) return;
    std::string path = std::string("/data/data/") + package + "/myfg_live";
    std::thread([path, on_applied] {
        name_thread("myfg-live");
        struct stat st {};
        bool have = stat(path.c_str(), &st) == 0;
        long long last_ns = have ? (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec : -1;
        off_t last_sz = have ? st.st_size : -1;       // файл, оставшийся от прошлого сеанса, не применяем
        for (;;) {
            usleep(1000 * 1000);
            if (!g_cfg.live) continue;
            if (stat(path.c_str(), &st) != 0) continue;
            long long ns = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
            if (ns == last_ns && st.st_size == last_sz) continue;
            last_ns = ns; last_sz = st.st_size;
            int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            std::string blob;
            char buf[4096];
            ssize_t r;
            while ((r = read(fd, buf, sizeof buf)) > 0 && blob.size() < 131072) blob.append(buf, (size_t)r);
            close(fd);
            Config n;                       // значения по умолчанию + то, что в файле (как при старте)
            n.parse(blob.c_str());
            const Config old = g_cfg;
            g_cfg.applyLive(n);
            std::string diff;
#define D(f) if (old.f != g_cfg.f) { char b[48]; snprintf(b, sizeof b, " %s=%d->%d", #f, old.f, g_cfg.f); diff += b; }
            D(enable) D(min_fps) D(max_dt_ms) D(acq_fence) D(zorder) D(display_fps) D(auto_rate) D(dataspace) D(hide_ms)
            D(release_lag) D(stats) D(max_gen) D(target_fps) D(flow_side) D(panel_hz) D(panel_game_vote) D(panel_anchor)
            D(multiplier) D(dup_detect) D(dup_thresh) D(use_oncomplete) D(layer_tf) D(hdr) D(hdr_pw) D(hdr_peak) D(hdr_knee) D(hdr_boost) D(hdr_gamma) D(hdr_ct) D(hdr_sat) D(adaptive)
            D(gpu_budget) D(log) D(debug) D(hud) D(hud_mode) D(hud_pos) D(hud_scale) D(hud_tf)
            D(thermal) D(thermal_hi) D(thermal_lo) D(thermal_hz)
            D(pts_phase) D(qcom_ex)
            D(pacing)
            D(target_w) D(target_h) D(elect_blast) D(ui_source)
#undef D
            if (!diff.empty()) LOGI("settings changed:%s", diff.c_str());
            if (on_applied) on_applied();
        }
    }).detach();
}

}  // namespace live
