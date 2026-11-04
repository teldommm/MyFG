// Zygisk-часть.
//
//  Кто что делает:
//   * Zygisk-модуль (этот код, в процессе zygote -> игры): preAppSpecialize определяет имя пакета и
//     спрашивает root-companion; postAppSpecialize в целевом процессе запускает myfg::start(),
//     в остальных просит Zygisk выгрузить библиотеку (DLCLOSE_MODULE_LIBRARY).
//   * companion (отдельный root-процесс): единственный, кто видит /data/adb. Читает packages.list
//     и config и отдаёт модулю. Из процесса игры /data/adb недоступен — отсюда и вся схема.
//
//  Протокол по сокету:  [u32 len][pkg]  ->  [u32 found][u32 len][config blob]
#include <jni.h>
#include <sys/types.h>
#include "zygisk.hpp"
#include "common.h"
#include "config.h"
#include "display.h"
#include "hooks.h"
#include "live.h"

#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <stdio.h>
#include <string>
#include <thread>
#include <unistd.h>

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

namespace {

bool read_full(int fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}
bool write_full(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}

// true — файл существует (даже пустой).
bool read_file_ex(const std::string& path, std::string* out, size_t cap = 65536) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[4096];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0 && out->size() < cap) out->append(buf, (size_t)r);
    close(fd);
    return true;
}

// Файл настроек относительно каталога модуля; если его там нет — из прежнего /data/adb/myfg.
std::string read_cfg(const std::string& rel) {
    std::string out;
    if (read_file_ex(std::string(MYFG_DIR "/") + rel, &out)) return out;
    read_file_ex(std::string(MYFG_DIR_LEGACY "/") + rel, &out);
    return out;
}

bool pkg_is_sane(const std::string& s) {
    if (s.empty() || s.size() > 200) return false;
    for (char c : s)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_')) return false;    // ':' (доп. процессы) и '/' не пускаем
    return true;
}

bool in_packages_list(const std::string& pkg) {
    std::string s = read_cfg("packages.list");
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        std::string line = s.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
        size_t a = 0;
        while (a < line.size() && isspace((unsigned char)line[a])) a++;
        line = line.substr(a);
        if (line.empty() || line[0] == '#') continue;
        if (line == pkg) return true;
    }
    return false;
}

// ---- companion (root) -------------------------------------------------------------------
void companion_handler(int fd) {
    uint32_t len = 0;
    if (!read_full(fd, &len, 4) || len == 0 || len > 200) return;
    std::string pkg(len, '\0');
    if (!read_full(fd, &pkg[0], len)) return;

    uint32_t found = 0;
    std::string cfg;
    if (pkg_is_sane(pkg) && in_packages_list(pkg)) {
        found = 1;
        cfg = read_cfg("config");
        cfg += "\n";
        cfg += read_cfg("config.d/" + pkg);       // переопределения профиля игры идут после глобальных
    }
    uint32_t clen = (uint32_t)cfg.size();
    write_full(fd, &found, 4);
    write_full(fd, &clen, 4);
    if (clen) write_full(fd, cfg.data(), clen);
}

// ---- модуль -----------------------------------------------------------------------------
class MyFgModule : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override { api_ = api; env_ = env; }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        if (!args->nice_name) return;
        const char* nn = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (!nn) return;
        pkg_ = nn;
        env_->ReleaseStringUTFChars(args->nice_name, nn);
        if (!pkg_is_sane(pkg_)) return;       // системные/доп. процессы и мусор отсекаем сразу

        int fd = api_->connectCompanion();
        if (fd < 0) return;
        uint32_t len = (uint32_t)pkg_.size(), found = 0, clen = 0;
        if (write_full(fd, &len, 4) && write_full(fd, pkg_.data(), len) &&
            read_full(fd, &found, 4) && read_full(fd, &clen, 4) && clen <= 65536 * 2) {
            if (found) {
                cfg_.assign(clen, '\0');
                if (clen && !read_full(fd, &cfg_[0], clen)) found = 0;
            }
            target_ = found != 0;
        }
        close(fd);
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!target_) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);   // нецелевой процесс: выгружаемся
            return;
        }
        JavaVM* vm = nullptr;
        if (env_ && env_->GetJavaVM(&vm) == 0) display::init(vm);   // HDR-возможности панели для SDR->HDR
        myfg::start(cfg_.c_str(), pkg_.c_str());
    }

private:
    Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
    std::string pkg_, cfg_;
    bool target_ = false;
};

}  // namespace

namespace myfg {
namespace {
std::string g_pkg;
std::atomic<bool> g_begun{false};

// Запускает всё, что нужно для работы (установка хуков и конвейер). Один раз за процесс.
void begin() {
    if (g_begun.exchange(true)) return;
    {
        char mode[24];
        if (g_cfg.multiplier >= 2) snprintf(mode, sizeof mode, "x%d", g_cfg.multiplier);
        else if (g_cfg.target_fps > 0) snprintf(mode, sizeof mode, "target %d fps", g_cfg.target_fps);
        else snprintf(mode, sizeof mode, "display rate");
        LOGI("%s: mode=%s max_gen=%d flow=%dpx min_fps=%d adaptive=%d(%d%%) hdr=%d panel=%dHz dup=%d live=%d",
             g_pkg.c_str(), mode, g_cfg.max_gen, g_cfg.flow_side, g_cfg.min_fps, g_cfg.adaptive, g_cfg.gpu_budget,
             g_cfg.hdr, g_cfg.panel_hz, g_cfg.dup_detect, g_cfg.live);
        LOGD("hooks: surface=%d egl=%d anw=%d vk=%d swap=%d present=%d fallback_ms=%d blast=%d delay=%dms | vote=%d anchor=%d onComplete=%d prio=%d layer_tf=%d",
             g_cfg.hook_surface, g_cfg.hook_egl, g_cfg.hook_anw, g_cfg.hook_vk, g_cfg.hook_swap, g_cfg.hook_present, g_cfg.fallback_ms,
             g_cfg.elect_blast, g_cfg.hook_delay_ms, g_cfg.panel_game_vote, g_cfg.panel_anchor, g_cfg.use_oncomplete, g_cfg.queue_prio, g_cfg.layer_tf);
    }
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50 + (g_cfg.hook_delay_ms > 0 ? g_cfg.hook_delay_ms : 0)));
        for (int i = 0; i < 400; i++) {                 // ~10 с по 100 мс, затем по 500 мс (до ~2 мин)
            if (hooks::tryInstall()) { LOGD("all hooks installed"); return; }
            usleep(i < 100 ? 100 * 1000 : 500 * 1000);
        }
        LOGW("hook installation finished with gaps: capture may not work (enable debug for details)");
    }).detach();
}

// Вызывается потоком live после каждого применённого обновления настроек.
void on_live_update() {
    if (g_cfg.enable) begin();      // игра стартовала с выключенным модулем, а потом его включили: запускаемся сейчас
}
}  // namespace

// Запускается в процессе игры. Хуки ставим из отдельного потока с повторами: в момент
// specialize нужные библиотеки (особенно libvulkan) могут быть ещё не загружены.
void start(const char* config_blob, const char* package) {
    g_cfg.parse(config_blob);
    g_pkg = package ? package : "";
    // live стартует всегда: иначе включение модуля в WebUI не действовало бы на игру, запущенную при enable=0.
    live::start(package, &on_live_update);
    if (!g_cfg.enable) { LOGI("%s: frame generation disabled in settings (waiting for the setting to be turned on)", package); return; }
    begin();
}
}  // namespace myfg

REGISTER_ZYGISK_MODULE(MyFgModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
