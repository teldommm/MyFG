/* MyFG WebUI.
 *
 * Хранение: всё лежит в каталоге модуля — /data/adb/modules/myfg/config/
 *   packages.list   игры (по пакету в строке)
 *   config          глобальные значения (key=value)
 *   config.d/<pkg>  переопределения одной игры (только отличающиеся ключи)
 * Эти же файлы читает root-companion модуля, поэтому формат менять нельзя.
 *
 * Доступ к файлам — через ksu.exec (KernelSU WebUI / WebUI X / MMRL). Вне менеджера (в обычном
 * браузере) работает демо-режим на памяти: ничего на устройстве не пишется.
 */
(() => {
'use strict';

const CFG_DIR = window.__MYFG_CFG_DIR || '/data/adb/modules/myfg/config';
const PROP    = window.__MYFG_PROP    || '/data/adb/modules/myfg/module.prop';
const LIVE    = window.__MYFG_LIVE    || '/data/adb/modules/myfg/live.sh';
const LICENSE_PATH = window.__MYFG_LICENSE || '/data/adb/modules/myfg/LICENSE';
const GLOBAL  = '@global';                          // '@' не бывает в имени пакета
const PKG_RE  = /^[A-Za-z0-9_][A-Za-z0-9._]{0,199}$/;   // то же правило, что в companion (main.cpp)

/* ══════════════════════════════════════════════════════════════════════════
   Схема настроек: одна запись — один ключ конфига. Значения по умолчанию
   совпадают с Config в src/config.h.
══════════════════════════════════════════════════════════════════════════ */
const SCHEMA = [
  { id: 'general', icon: '⚙️', title: 'g_general', items: [
    { k: 'enable', t: 'bool', def: 1, icon: '⚡' },
  ]},
  // ── Основное: всегда на виду ──
  { id: 'gen', icon: '🎞️', title: 'g_gen', custom: 'mode', items: [
    { k: 'multiplier', t: 'num', def: 0, min: 0, max: 8, smin: 2, sl: 1, unit: '×', icon: '✖️', show: 'mult', quick: [2, 3, 4, 6] },
    { k: 'target_fps', t: 'num', def: 0, min: 0, max: 480, smin: 20, smax: 240, sl: 1, unit: 'fps', icon: '🎯', show: 'target', quick: [30, 45, 60, 90, 120, 144] },
    { k: 'max_gen',    t: 'num', def: 3, min: 0, max: 7, sl: 1, unit: '', icon: '🔢', show: ['auto', 'target'] },
  ]},
  { id: 'load', icon: '🌡️', title: 'g_load', custom: 'preset', items: [
    { k: 'adaptive',   t: 'bool', def: 1, icon: '🛟' },
    { k: 'flow_side',  t: 'num', def: 180, min: 64, max: 1080, step: 8, sl: 1, unit: 'px', icon: '🔍', quick: [128, 180, 270, 360], vis: 'custom' },
    { k: 'gpu_budget', t: 'num', def: 50, min: 10, max: 90, step: 5, sl: 1, unit: '%', icon: '⚖️', dep: [['adaptive', '=', 1]], vis: 'custom' },
    { k: 'thermal',    t: 'bool', def: 0, icon: '🔋', dyn: 'thermhint' },
    { k: 'thermal_hi', t: 'num', def: 44, min: 35, max: 60, sl: 1, unit: '°C', icon: '🌡️', quick: [40, 42, 44, 46, 48], dep: [['thermal', '=', 1]] },
    { k: 'thermal_lo', t: 'num', def: 40, min: 25, max: 59, sl: 1, unit: '°C', icon: '❄️', dep: [['thermal', '=', 1]] },
    { k: 'thermal_hz', t: 'num', def: 60, min: 30, max: 144, smax: 144, sl: 1, unit: 'Hz', icon: '📉', quick: [30, 60, 90], dep: [['thermal', '=', 1]] },
  ]},
  { id: 'hdr', icon: '🌈', title: 'g_hdr', items: [
    { k: 'hdr',       t: 'bool', def: 0, icon: '🌈', dyn: 'hdrhint' },
    { k: 'hdr_pw',    t: 'num', def: 200, min: 100, max: 1000, step: 10, sl: 1, unit: 'nit', icon: '💡', quick: [150, 200, 300, 500], dep: [['hdr', '=', 1]] },
    { k: 'hdr_boost', t: 'num', def: 30, min: 0, max: 100, step: 5, sl: 1, unit: '%', icon: '✨', dep: [['hdr', '=', 1]] },
    { k: 'hdr_gamma', t: 'num', def: 22, min: 0, max: 26, smin: 18, div: 10, sl: 1, zsnap: 18, unit: '', icon: '🌓', z: 'z_srgb', dep: [['hdr', '=', 1]] },
    { k: 'hdr_ct',    t: 'num', def: 100, min: 50, max: 150, step: 5, sl: 1, unit: '%', icon: '◐', dep: [['hdr', '=', 1]] },
    { k: 'hdr_sat',   t: 'num', def: 100, min: 50, max: 150, step: 5, sl: 1, unit: '%', icon: '🎨', dep: [['hdr', '=', 1]] },
  ]},
  { id: 'panel', icon: '📟', title: 'g_panel', items: [
    { k: 'panel_hz', t: 'num', def: 120, min: 0, max: 480, smax: 240, sl: 1, unit: 'Hz', icon: '🔄', z: 'z_off', quick: () => panelQuick() },
  ]},
  { id: 'hud', icon: '📊', title: 'g_hud', items: [
    { k: 'hud',       t: 'bool', def: 0, icon: '📊' },
    { k: 'hud_mode',  t: 'mode', def: 2, icon: '🧾', opts: ['hm_fps', 'hm_arrow', 'hm_full'], dep: [['hud', '=', 1]] },
    { k: 'hud_pos',   t: 'mode', def: 0, icon: '📍', opts: ['hp_tl', 'hp_tr', 'hp_bl', 'hp_br'], dep: [['hud', '=', 1]] },
    { k: 'hud_scale', t: 'num',  def: 3, min: 1, max: 8, sl: 1, unit: '×', icon: '🔠', dep: [['hud', '=', 1]] },
  ]},
  // ── Тонкая настройка: свёрнута ──
  { id: 'fine', icon: '🎚️', title: 'g_fine', adv: 1, items: [
    { k: 'min_fps',   t: 'num', def: 0, min: 0, max: 240, smin: 20, smax: 60, sl: 1, unit: 'fps', icon: '🐢', z: 'z_off', quick: [0, 25, 30, 40] },
    { k: 'pts_phase', t: 'bool', def: 1, icon: '⏱️', dyn: 'ptshint' },
    { k: 'dup_detect', t: 'bool', def: 0, icon: '👯' },
    { k: 'dup_thresh', t: 'pick', def: 2, min: 0, max: 64, vals: [0, 2, 6], opts: ['pk_exact', 'pk_normal', 'pk_soft'], icon: '📏', dep: [['dup_detect', '=', 1]], dyn: 'duphint' },
    { k: 'qcom_me',   t: 'bool', def: 0, icon: '🧲', dyn: 'mehint' },
    { k: 'me_refine', t: 'pick', def: 1, min: 0, max: 2, vals: [0, 1, 2], opts: ['mr_off', 'mr_light', 'mr_full'], icon: '🧹', dep: [['qcom_me', '=', 1]] },
    { k: 'me_dy_sign', t: 'pick', def: 0, min: 0, max: 2, vals: [0, 1, 2], opts: ['mds_auto', 'mds_down', 'mds_up'], icon: '🧭', dep: [['qcom_me', '=', 1]] },
    { k: 'panel_game_vote', t: 'bool', def: 1, icon: '🗳️', dep: [['panel_hz', '>', 0]] },
    { k: 'panel_anchor',    t: 'bool', def: 0, icon: '⚓', dep: [['panel_hz', '>', 0]] },
    { k: 'hdr_peak', t: 'num', def: 0, min: 0, max: 1400, step: 50, zsnap: 400, sl: 1, smin: 400, unit: 'nit', icon: '🔆', z: 'z_auto', quick: [0, 600, 800, 1000, 1200], dep: [['hdr', '=', 1]] },
    { k: 'hdr_knee', t: 'num', def: 60, min: 10, max: 100, step: 5, sl: 1, unit: '%', icon: '📈', dep: [['hdr', '=', 1]] },
    { k: 'layer_tf', t: 'pick', def: -1, min: -1, max: 7, vals: [-1, 0, 4, 3, 7, 1, 2], opts: ['z_game', '0°', '90°', '180°', '270°', '⇄', '⇅'], wrap: 1, icon: '🔃' },
    { k: 'hud_tf',   t: 'pick', def: 0, min: 0, max: 7, vals: [0, 4, 3, 7, 1, 2], opts: ['0°', '90°', '180°', '270°', '⇄', '⇅'], wrap: 1, icon: '🔄', dep: [['hud', '=', 1]] },
  ]},
  // ── Экспертные: скрыты, пока не включён переключатель ──
  { id: 'out', icon: '🖥️', title: 'g_out', adv: 1, expert: 1, items: [
    { k: 'use_oncomplete', t: 'bool', def: 1, icon: '✅' },
    { k: 'pacing', t: 'bool', def: 1, icon: '🎛️', dep: [['use_oncomplete', '=', 1]] },
    { k: 'release_lag', t: 'num',  def: 3, min: 0, max: 8, sl: 1, unit: '', icon: '🧮', dep: [['use_oncomplete', '=', 0]] },
  ]},
  { id: 'cap', icon: '📥', title: 'g_cap', adv: 1, expert: 1, after: 'queue', items: [
    { k: 'acq_fence', t: 'bool', def: 1, icon: '🚧' },
    { k: 'max_dt_ms', t: 'num',  def: 120, min: 50, max: 2000, smax: 500, step: 10, sl: 1, unit: 'ms', icon: '⏱️' },
    { k: 'target_w',  t: 'num',  def: 0, min: 0, max: 16384, step: 16, unit: 'px', icon: '↔️', z: 'z_auto' },
    { k: 'target_h',  t: 'num',  def: 0, min: 0, max: 16384, step: 16, unit: 'px', icon: '↕️', z: 'z_auto' },
    { k: 'elect_blast', t: 'bool', def: 1, icon: '🧭' },
    { k: 'hook_bbq', t: 'bool', def: 1, icon: '🎞️' },
    { k: 'ui_source', t: 'bool', def: 0, icon: '🪟' },
    { k: 'hook_swap',    t: 'mode', def: 1, icon: '🧩' },
    { k: 'hook_present', t: 'mode', def: 1, icon: '🧩' },
    { k: 'fallback_ms',  t: 'num',  def: 10000, min: 1000, max: 120000, step: 1000, unit: 'ms', icon: '⏲️', dep: [['hook_swap', '=', 1], ['hook_present', '=', 1]] },
  ]},
  { id: 'adv', icon: '🧪', title: 'g_adv', note: 'advNote', custom: 'compat', adv: 1, expert: 1, items: [
    { k: 'hook_surface',  t: 'bool', def: 1, icon: '🪝' },
    { k: 'hook_egl',      t: 'mode', def: 1, icon: '🪟' },
    { k: 'hook_anw',      t: 'mode', def: 1, icon: '🪟' },
    { k: 'hook_vk',       t: 'bool', def: 0, icon: '⚠️' },
    { k: 'hook_delay_ms', t: 'num',  def: 0, min: 0, max: 60000, step: 500, unit: 'ms', icon: '⏳' },
    { k: 'queue_prio',    t: 'bool', def: 1, icon: '🚀' },
    { k: 'debug',         t: 'bool', def: 0, icon: '🐞' },
  ]},
];
// Ключи без элементов управления: значения по умолчанию подходят всем. Остаются в файлах (их читает модуль и
// ручные правки не теряются), но в интерфейсе их нет.
const HIDDEN = [
  { k: 'display_fps', t: 'num',  def: 0, min: 0, max: 480 },
  { k: 'auto_rate',   t: 'bool', def: 0 },
  { k: 'live',        t: 'bool', def: 1 },
  { k: 'stats',       t: 'bool', def: 1 },
  { k: 'zorder',      t: 'num',  def: 1, min: -8, max: 8 },
  { k: 'dataspace',   t: 'bool', def: 1 },
  { k: 'hide_ms',     t: 'num',  def: 300, min: 0, max: 10000 },
];
// Режим совместимости: готовые наборы ключей хуков (порядок поиска причины зависания из README).
const COMPAT = {
  std:  { hook_delay_ms: 0,    hook_surface: 1, hook_egl: 1, hook_anw: 1 },
  soft: { hook_delay_ms: 3000, hook_surface: 1, hook_egl: 1, hook_anw: 1 },
  min:  { hook_delay_ms: 0,    hook_surface: 0, hook_egl: 0, hook_anw: 0 },
};
const PRESETS = {
  perf: { flow_side: 128, gpu_budget: 40, adaptive: 1 },
  bal:  { flow_side: 180, gpu_budget: 50, adaptive: 1 },
  qual: { flow_side: 270, gpu_budget: 60, adaptive: 1 },
};
const ALL_ITEMS = [...SCHEMA.flatMap(g => g.items), ...HIDDEN];
const BY_KEY = Object.fromEntries(ALL_ITEMS.map(i => [i.k, i]));

/* ══════════════════════════════════════════════════════════════════════════
   Тексты
══════════════════════════════════════════════════════════════════════════ */
const T = {
ru: {
  loading: 'загрузка…', on: 'вкл', off: 'выкл', preview: 'демо',
  scope: 'Профиль', scopeGlobal: 'Глобальные (все игры)',
  noteGlobal: 'Глобальные значения действуют для всех игр из списка. Большинство настроек применяется сразу в запущенной игре; хуки, очередь и размеры — при следующем запуске.',
  notePkg: 'Профиль игры: подсвеченные строки переопределяют глобальные, ↺ возвращает глобальное значение. Большинство настроек применяется сразу; хуки — при следующем запуске.',
  restartGame: 'Перезапустить игру', stopped: 'Игра остановлена',
  games: 'Игры', add: 'Добавить', pickInstalled: 'Выбрать из установленных',
  gamesNote: 'Модуль работает только в играх из этого списка. Нажмите на игру, чтобы открыть её профиль.',
  noGames: 'Список пуст — добавьте игру', phPackage: 'com.example.game', phSearch: 'Поиск',
  profileYes: 'есть свои настройки', profileNo: 'настройки как у глобальных', del: 'Удалить?',
  pickTitle: 'Установленные приложения', pickHint: 'Сторонние приложения. Уже добавленные отмечены.',
  addSelected: 'Добавить выбранные', close: 'Закрыть', added: 'добавлена', nothing: 'Ничего не найдено',
  badPkg: 'Неверное имя пакета', dup: 'Уже в списке', pkgAdded: 'Добавлено', pkgRemoved: 'Удалено из списка',
  saved: 'Сохранено', saveFail: 'Не удалось сохранить',
  status: 'Статус и лог', refresh: 'Обновить', copy: 'Копировать', copied: 'Скопировано', logEmpty: 'Лога myfg пока нет — запустите игру из списка.',
  mSrc: 'источник', mGen: 'генерация', mShown: 'вывод', mUnit: 'к/с',
  restartNote: '⟳ Применится после перезапуска игры',
  mStopped: 'игра не запущена', mPaused: 'генерация выключена',
  logRun: g => g ? `Лог запущенной игры: ${g}` : 'Лог запущенной игры', logStopped: g => g ? `Игра не запущена — ниже лог её последнего запуска (${g})` : 'Игра не запущена — ниже общий лог модуля',
  dropLate: 'догон', dropOut: 'потеряно', dropReal: 'реал. пропущено', importFail: 'ошибок импорта', hidden: 'оверлей скрыт',
  modeOff: 'Выкл', modeFallback: 'Запасной', modeAlways: 'Всегда',
  g_panel: 'Частота панели', g_load: 'Нагрузка на GPU', g_hdr: 'HDR',
  k_adaptive: 'Адаптивная нагрузка', d_adaptive: 'Мерит время GPU нашего прохода; при перегрузе снижает разрешение потока, затем число сгенерированных кадров, при запасе возвращает',
  k_gpu_budget: 'Бюджет GPU', d_gpu_budget: 'Какую долю интервала между кадрами игры модуль может занимать на GPU (%); меньше — раньше сработает снижение',
  k_thermal: 'Троттлинг по температуре', d_thermal: 'При перегреве батареи вывод деградирует до заданной частоты: панель реально уходит вниз (экономия и без троттлинга SoC), при остывании полный темп возвращается.',
  k_thermal_hi: 'Порог перегрева', d_thermal_hi: 'Температура батареи, °C, выше которой (два замера подряд) срабатывает деградация',
  k_thermal_lo: 'Порог восстановления', d_thermal_lo: 'Ниже этой температуры полный темп возвращается; держите ниже порога перегрева — это гистерезис от мигания туда-сюда',
  k_thermal_hz: 'Частота при перегреве', d_thermal_hz: 'До скольки Гц опускать вывод при перегреве: генерация урезается так, чтобы выход не превышал эту частоту',
  k_hdr: 'SDR → HDR', d_hdr: 'Каждый выходной кадр перекодируется в HDR10 (BT.2020 + PQ), слой помечается HDR. Нужна панель с HDR; цена — один лёгкий проход на кадр',
  k_hdr_pw: 'Яркость белого', d_hdr_pw: 'До скольких нит поднимается белый цвет SDR (paper white), 100…1000. Больше — ярче вся картинка',
  k_hdr_boost: 'Усиление светов', d_hdr_boost: 'Насколько яркие места растягиваются к пику панели: 0 — белый остаётся на яркости белого, 100 — белый достигает пика',
  k_hdr_gamma: 'Гамма · глубина теней', d_hdr_gamma: 'Гамма, по которой SDR переводится в линейный свет, 1.8…2.6 (по умолчанию 2.2); «sRGB» — кривая sRGB. Больше — глубже тени',
  k_hdr_ct: 'Контраст', d_hdr_ct: 'S-образная кривая: 100% — без изменений; чёрное и белое остаются на месте',
  k_hdr_sat: 'Насыщенность', d_hdr_sat: 'Яркость цветов, 50…150% (100% — как в игре)',
  k_hdr_peak: 'Пик панели', d_hdr_peak: 'Максимальная яркость, до которой можно растягивать света, нит. «Авто» — значение панели (если не сообщила, 1000)',
  k_hdr_knee: 'Колено светов', d_hdr_knee: 'С какой яркости (в % от белого) начинается растяжение светов; ниже — картинка не меняется по форме',
  k_multiplier: 'Множитель', d_multiplier: 'Во сколько раз чаще кадры на выходе: ×2 — один добавленный кадр между реальными. Не больше, чем покажет экран',
  k_dup_detect: 'Детект повторных кадров', d_dup_detect: 'Игра постит 60 к/с при 30 уникальных: повторы пропускаются. Цена: ожидание на CPU каждый кадр',
  k_pts_phase: 'Фаза по меткам источника', d_pts_phase: 'Интервал и якорь вывода берутся из timestamp кадров (PTS видео), а не из моментов прибытия: убирает дрожание фазы (judder) на 24/30 fps → 120 Гц.',
  k_qcom_me: 'Аппаратный ME (Adreno)', d_qcom_me: 'Вектора движения из GL_QCOM_motion_estimation: почти бесплатно. DIS остаётся запасным. Пока экспериментально, для теста качества',
  k_me_refine: 'Уточнение поля ME', d_me_refine: 'GPU-доводка аппаратного поля: медиана временного поля + gain-компенсация экспозиции (фейды/вспышки не ломают поиск) + защита центра экрана (HUD). «Лёгкое» — 4-угловая SAD, «Полное» — гексагональный поиск 34 кандидата (как в проде донора)',
  mr_off: 'Выкл', mr_light: 'Лёгкое', mr_full: 'Полное',
  k_me_dy_sign: 'Знак Y поля ME', d_me_dy_sign: 'Спека QCOM motion estimation не определяет направление оси Y векторов. По умолчанию при старте игры модуль прогоняет самотест на синтетике (logcat: «dy self-test») и приводит поле к координатам содержимого. Если самотест неубедителен, а картинка при включённом ME «плывёт» по вертикали — задайте знак вручную: «Y вниз» (без коррекции) или «Y вверх» (инверсия). Меняется на ходу',
  mds_auto: 'Самотест', mds_down: 'Y вниз', mds_up: 'Y вверх',
  k_pacing: 'Пейсинг с обратной связью', d_pacing: 'По лэтчу транзакций (onComplete) меряется фактический момент показа; стабильное опоздание плавно разрешает более ранние таймлайны (до +2.5 мс), при запасе надбавка снимается. Работает при «Освобождение по onComplete» и на Android 13+ (таймлайны)',
  dyn_ptshint: 'По логу часть кадров приходит без метки (PTS): для них фаза берётся по моменту прибытия, как раньше.',
  dyn_mehint: 'По логу аппаратный ME недоступен (нет расширения или ошибка GL): поток считает DIS.',
  dyn_thermhint: 'По логу датчика температуры батареи нет — троттлинг неактивен, вывод всегда на полном темпе.',
  k_dup_thresh: 'Порог повтора', d_dup_thresh: 'Насколько кадры должны различаться, чтобы считаться разными: «Точные» пропускают только идентичные, «Мягкий» — и почти одинаковые',
  k_panel_hz: 'Частота панели', d_panel_hz: 'Подсказка SurfaceFlinger для слоя и окна игры; 0 — не трогать. Быстрые значения — режимы вашего экрана',
  k_panel_game_vote: 'Голос на окно игры', d_panel_game_vote: 'Ту же частоту ставить окну игры (ANativeWindow_setFrameRate)',
  k_panel_anchor: 'Слой-якорь', d_panel_anchor: 'Отдельный прозрачный слой обновляется с частотой панели и держит режим, даже когда оверлей молчит',
  k_use_oncomplete: 'Освобождение по onComplete', d_use_oncomplete: 'Буфер свободен, когда SurfaceFlinger отдал его; выкл — по «Удержанию буферов»',
  k_layer_tf: 'Поворот слоя', d_layer_tf: 'Если картинка повёрнута или отражена (игры с pre-rotation) — выберите угол или зеркало. «Из игры» — по умолчанию',
  k_elect_blast: 'Подсказки BLASTBufferQueue', d_elect_blast: 'Размеры окон из BLASTBufferQueue помогают выбрать игровую очередь (хук при запуске)',
  k_hook_bbq: 'Окно видео через BLAST', d_hook_bbq: 'Видео и кадры из GPU-процесса (Binder): слой оверлея встаёт ровно на SurfaceView и остаётся под интерфейсом. Выкл. — такие источники не обрабатываются',
  k_ui_source: 'Окно приложения как источник', d_ui_source: 'Разрешить брать кадры интерфейса приложения (HWUI). Обычно не нужно: закрывает видео чёрным прямоугольником',
  k_hook_swap: 'Запасной захват: eglSwapBuffers', d_hook_swap: 'GLES-игры, если BufferQueue не отдаёт кадры. «Запасной» — включается через время ниже; «Всегда» — хук ставится сразу. Хук — при запуске',
  k_hook_present: 'Запасной захват: vkQueuePresentKHR', d_hook_present: 'Vulkan-игры. Хук vkCreateDevice должен стоять до создания устройства: «Запасной» — только если BufferQueue-хуки не встали, «Всегда» — всегда. Применяется при запуске',
  k_fallback_ms: 'Включать запасной захват через', d_fallback_ms: 'Сколько мс без кадров от BufferQueue после старта; запасной путь молчит, пока BufferQueue работает',
  k_queue_prio: 'Высокий приоритет очереди', d_queue_prio: 'Просить у драйвера высокий приоритет очереди модуля; при отказе — обычный. Применяется при запуске',
  k_live: 'Применять без перезапуска', d_live: 'Игра подхватывает безопасные настройки сразу после сохранения (хуки и очередь — только при запуске)',
  advNote: 'Менять только при диагностике. hook_vk может подвесить игру при старте.',
  m_title: 'Режим генерации', m_desc: 'Как определяется, сколько кадров добавлять между кадрами игры',
  m_auto: 'Авто', m_mult: 'Множитель', m_target: 'Цель FPS',
  s_auto: () => 'Вывод подстраивается под частоту экрана: чем меньше FPS у игры, тем больше кадров добавляется.',
  s_mult: n => `×${n}: между кадрами игры вставляется ${n - 1} кадр(а) — вывод чаще в ×${n}, но не выше частоты экрана.`,
  s_target: (f, g) => `Вывод тянется до ${f} FPS; число добавляемых кадров подбирается само, но не больше ${g} на кадр игры.`,
  s_off: () => 'Макс. генерируемых = 0: кадры не добавляются, модуль только пробрасывает картинку.',
  p_title: 'Качество и скорость', p_desc: 'Быстрый выбор качества потока и бюджета GPU. «Свой» открывает ручные ползунки.',
  p_perf: 'Скорость', p_bal: 'Баланс', p_qual: 'Качество', p_custom: 'Свой',
  z_srgb: 'sRGB', z_auto: 'авто', z_off: 'выкл', z_never: 'никогда', z_game: 'из игры', z_unset: 'нет',
  resetOne: 'Вернуть глобальное значение', resetCard: 'Сбросить группу к значениям по умолчанию', confirmReset: 'Сбросить?', resetDone: 'Сброшено',
  autoRefresh: 'Авто', mIdle: 'игра не шлёт кадры', mGpu: 'GPU', mGov: 'снижение нагрузки', mWarn: 'предупреждений', mHdr: 'HDR',
  k_debug: 'Подробный лог', d_debug: 'Адреса, форматы, первые кадры; статистика раз в секунду. Включать только для диагностики',
  g_hud: 'Счётчик FPS',
  hm_fps: 'FPS', hm_arrow: 'Игра→вывод', hm_full: 'Подробно', hp_tl: '↖', hp_tr: '↗', hp_bl: '↙', hp_br: '↘',
  k_hud: 'Счётчик FPS поверх игры', d_hud: 'Свой слой поверх игры показывает, сколько кадров в секунду реально выводится и сколько из них добавил модуль',
  k_hud_mode: 'Что показывать', d_hud_mode: 'FPS — итоговая частота; Игра→вывод — «30>120»; Подробно — FPS, GAME (кадры игры), GEN (добавлено модулем), GPU (время прохода)',
  k_hud_pos: 'Угол экрана', d_hud_pos: 'Где расположен счётчик',
  k_hud_scale: 'Размер', d_hud_scale: 'Масштаб текста, 1…8',
  k_hud_tf: 'Поворот счётчика', d_hud_tf: 'Если счётчик повёрнут — выберите угол. Обычно 0°',
  licenseHint: 'Модуль распространяется на условиях GNU GPL версии 3. Полный текст лежит в файле LICENSE в каталоге модуля.',
  licenseMissing: 'Файл LICENSE не найден. Текст: https://www.gnu.org/licenses/gpl-3.0.txt',
  g_general: 'Модуль', g_gen: 'Генерация кадров', g_out: 'Освобождение буферов', g_cap: 'Захват', g_adv: 'Хуки и диагностика',
  k_enable: 'Генерация кадров', d_enable: 'Главный выключатель (для профиля игры — только для неё)',
  k_max_gen: 'Макс. генерируемых кадров', d_max_gen: 'Потолок на один кадр игры в режимах «Авто» и «Цель FPS»: 0 — только проброс, до 7. В режиме «Множитель» не используется',
  k_target_fps: 'Целевой FPS', d_target_fps: 'До какого FPS дотягивать вывод; экран не покажет больше своей частоты',
  k_flow_side: 'Разрешение потока', d_flow_side: 'Меньшая сторона для оптического потока: меньше — быстрее, больше — точнее',
  k_max_dt_ms: 'Макс. пауза между кадрами', d_max_dt_ms: 'Одиночная пауза между кадрами дольше этого — кадр идёт без интерполяции (подвисание). Минимум 50 мс: меньшее значение отключает генерацию',
  k_min_fps: 'Мин. FPS источника', d_min_fps: 'Ниже этого FPS игры генерация выключена. Значения меньше 20 ничего не меняют: WFG и так выключается ниже 20 и включается выше 24',
  k_zorder: 'Z-порядок слоя', d_zorder: 'Положение оверлея относительно окна игры',
  k_display_fps: 'Частота слоя (фикс.)', d_display_fps: 'Подсказка SurfaceFlinger; 0 — не задавать',
  k_auto_rate: 'Авто-частота слоя', d_auto_rate: 'FPS источника × (кадры+1), если фиксированная не задана. Влияет на режим панели',
  k_dataspace: 'Цветовое пространство', d_dataspace: 'Передавать dataspace игры оверлею',
  k_hide_ms: 'Прятать оверлей без кадров', d_hide_ms: 'Через сколько мс без кадров виден слой игры; 0 — не прятать',
  k_release_lag: 'Удержание буферов', d_release_lag: 'Сколько показов держать кадр после вывода (0–8)',
  k_acq_fence: 'Ждать готовности кадра', d_acq_fence: 'Acquire-fence игры перед копированием',
  k_target_w: 'Ширина буфера игры', d_target_w: 'Принудительный выбор очереди по размеру; 0 — авто',
  k_target_h: 'Высота буфера игры', d_target_h: 'Принудительный выбор очереди по размеру; 0 — авто',
  k_hook_surface: 'Хук Surface', d_hook_surface: 'Окно, буфер и fence из Surface::hook_queueBuffer (рекомендуется)',
  k_hook_egl: 'Запасной путь: EGL', d_hook_egl: 'Окно через eglCreateWindowSurface: «Запасной» — только если хук Surface не встал',
  k_hook_anw: 'Запасной путь: ANativeWindow', d_hook_anw: 'Окно через ANativeWindow_fromSurface: «Запасной» — только если хук Surface не встал',
  k_hook_vk: 'Патч свопчейна (Vulkan)', d_hook_vk: 'Опасно: хук libvulkan может подвесить игру при старте',
  k_hook_delay_ms: 'Задержка установки хуков', d_hook_delay_ms: 'Пауза после старта процесса, мс (поиск причины зависаний)',
  k_stats: 'Статистика в logcat', d_stats: 'Строка раз в 5 секунд, пока игра шлёт кадры: logcat -s myfg',
  g_fine: 'Тонкая настройка',
  x_title: 'Экспертные настройки', x_desc: 'Захват кадров, хуки и диагностика. Нужны, только если игра не запускается или не отдаёт кадры', x_changed: 'изменено',
  c_title: 'Режим совместимости', c_desc: 'Если игра зависает на заставке — попробуйте по очереди «Мягкий», затем «Минимальный». Применяется при следующем запуске игры',
  c_std: 'Стандартный', c_soft: 'Мягкий', c_min: 'Минимальный',
  c_s_std: 'Все хуки включены, без задержки.',
  c_s_soft: 'Хуки ставятся через 3 секунды после старта процесса — обходит гонку с потоками игры при запуске.',
  c_s_min: 'Только хуки BufferQueue: без хука Surface и без поиска окна через EGL/ANativeWindow.',
  c_s_custom: 'Своя комбинация хуков и задержки (настройки ниже).',
  q_fromLog: 'Взять из лога', q_auto: 'Авто', q_set: 'Очередь зафиксирована', q_none: 'В логе нет выбранной очереди — запустите игру и повторите',
  q_note: 'Фиксирует размер игровой очереди по последнему запуску (строка «game queue elected» в логе). «Авто» — модуль выбирает сам.',
  a_plan: (s, o, g) => `По логу: игра ≈${s} → вывод ≈${o} к/с, +${g} кадр. на кадр игры.`,
  a_plan0: s => `По логу: игра ≈${s} к/с — кадры не добавляются (FPS игры ниже порога 20–24 или вывод уже на частоте экрана).`,
  a_stats: (s, o) => `По логу: игра ≈${s} → вывод ≈${o} к/с.`,
  gov_now: (l, f) => `Регулятор нагрузки снизил разрешение потока до ≈${f} px (уровень ${l}).`,
  gov_now0: l => `Регулятор нагрузки снизил разрешение потока (уровень ${l}).`,
  dyn_hdrhint: 'По логу панель не сообщает поддержку HDR — вывод остаётся SDR. Если телефон на самом деле умеет HDR, включите HDR в системных настройках экрана.',
  dyn_duphint: 'По логу детектор повторов недоступен: буфер игры имеет внешний формат (YCbCr).',
  pk_exact: 'Точные', pk_normal: 'Обычный', pk_soft: 'Мягкий',
},
en: {
  loading: 'loading…', on: 'on', off: 'off', preview: 'preview',
  scope: 'Profile', scopeGlobal: 'Global (all games)',
  noteGlobal: 'Global values apply to every game in the list. Most settings apply instantly in a running game; hooks, queue and sizes on the next launch.',
  notePkg: 'Game profile: highlighted rows override the global ones, ↺ restores the global value. Most settings apply instantly; hooks on the next launch.',
  restartGame: 'Restart game', stopped: 'Game stopped',
  games: 'Games', add: 'Add', pickInstalled: 'Pick from installed apps',
  gamesNote: 'The module only runs in games from this list. Tap a game to open its profile.',
  noGames: 'The list is empty — add a game', phPackage: 'com.example.game', phSearch: 'Search',
  profileYes: 'has its own settings', profileNo: 'same as global', del: 'Remove?',
  pickTitle: 'Installed apps', pickHint: 'Third-party apps. Already added ones are marked.',
  addSelected: 'Add selected', close: 'Close', added: 'added', nothing: 'Nothing found',
  badPkg: 'Invalid package name', dup: 'Already in the list', pkgAdded: 'Added', pkgRemoved: 'Removed from the list',
  saved: 'Saved', saveFail: 'Could not save',
  status: 'Status & log', refresh: 'Refresh', copy: 'Copy', copied: 'Copied', logEmpty: 'No myfg log yet — launch a game from the list.',
  mSrc: 'source', mGen: 'generated', mShown: 'shown', mUnit: 'fps',
  restartNote: '⟳ Takes effect after the game is restarted',
  mStopped: 'game not running', mPaused: 'generation off',
  logRun: g => g ? `Log of the running game: ${g}` : 'Log of the running game', logStopped: g => g ? `Game not running — showing the log of its last run (${g})` : 'Game not running — showing the module log',
  dropLate: 'catch-up', dropOut: 'lost', dropReal: 'real skipped', importFail: 'import errors', hidden: 'overlay hidden',
  modeOff: 'Off', modeFallback: 'Fallback', modeAlways: 'Always',
  g_panel: 'Panel refresh rate', g_load: 'GPU load', g_hdr: 'HDR',
  k_adaptive: 'Adaptive load', d_adaptive: 'Measures the GPU time of our pass; on overload lowers the flow resolution, then the number of generated frames, and restores them when there is headroom',
  k_gpu_budget: 'GPU budget', d_gpu_budget: 'Share of the game frame interval the module may spend on the GPU (%); lower triggers the reduction sooner',
  k_thermal: 'Thermal throttling', d_thermal: 'When the battery overheats the output degrades to the set rate: the panel actually drops (savings even before SoC throttling), and the full rate returns as it cools.',
  k_thermal_hi: 'Overheat threshold', d_thermal_hi: 'Battery temperature, °C, above which (two consecutive readings) the degradation kicks in',
  k_thermal_lo: 'Recovery threshold', d_thermal_lo: 'Below this temperature the full rate returns; keep it under the overheat threshold — the hysteresis prevents flapping',
  k_thermal_hz: 'Rate when overheated', d_thermal_hz: 'The output rate to cap at when overheated: generation is trimmed so the output never exceeds it',
  k_hdr: 'SDR → HDR', d_hdr: 'Every output frame is re-encoded to HDR10 (BT.2020 + PQ) and the layer is tagged as HDR. Needs an HDR panel; costs one light pass per frame',
  k_hdr_pw: 'White brightness', d_hdr_pw: 'How many nits SDR white is raised to (paper white), 100…1000. Higher — a brighter picture overall',
  k_hdr_boost: 'Highlight boost', d_hdr_boost: 'How far bright areas are stretched toward the panel peak: 0 — white stays at paper white, 100 — white reaches the peak',
  k_hdr_gamma: 'Gamma · shadow depth', d_hdr_gamma: 'Gamma used to turn SDR into linear light, 1.8…2.6 (default 2.2); “sRGB” — the sRGB curve. Higher — deeper shadows',
  k_hdr_ct: 'Contrast', d_hdr_ct: 'S-curve: 100% — unchanged; black and white stay where they are',
  k_hdr_sat: 'Saturation', d_hdr_sat: 'Colour intensity, 50…150% (100% — as the game draws it)',
  k_hdr_peak: 'Panel peak', d_hdr_peak: 'Maximum brightness highlights may be stretched to, in nits. “Auto” — what the panel reports (1000 if it does not)',
  k_hdr_knee: 'Highlight knee', d_hdr_knee: 'Brightness (as % of white) where highlight stretching starts; below it the tone shape is untouched',
  k_multiplier: 'Multiplier', d_multiplier: 'How many times more frames on output: ×2 = one added frame between real ones. Capped by what the display can show',
  k_dup_detect: 'Duplicate-frame detection', d_dup_detect: 'Game posts 60 fps with 30 unique: repeats are skipped. Cost: a CPU wait every frame',
  k_pts_phase: 'Phase from source timestamps', d_pts_phase: 'Output spacing and anchor come from the frame timestamps (video PTS) instead of arrival instants: removes phase jitter (judder) at 24/30 fps → 120 Hz.',
  k_qcom_me: 'Hardware ME (Adreno)', d_qcom_me: 'Motion vectors from GL_QCOM_motion_estimation: nearly free. DIS stays the fallback. Experimental, for quality testing',
  k_me_refine: 'ME field refinement', d_me_refine: 'GPU polish of the hardware field: temporal-field median + exposure gain compensation (fades/flashes no longer break the search) + centre-of-screen (HUD) protection. “Light” — 4-corner SAD, “Full” — 34-candidate hex search (as in the donor\u2019s production build)',
  mr_off: 'Off', mr_light: 'Light', mr_full: 'Full',
  k_me_dy_sign: 'ME field Y-axis sign', d_me_dy_sign: 'The QCOM motion estimation spec does not define the Y-axis direction of the vectors. By default the module runs a synthetic self-test at game start (logcat: “dy self-test”) and converts the field to content coordinates. If the test is inconclusive and the picture drifts vertically with ME enabled — set the sign manually: “Y down” (no correction) or “Y up” (inverted). Applies live',
  mds_auto: 'Self-test', mds_down: 'Y down', mds_up: 'Y up',
  k_pacing: 'Feedback pacing', d_pacing: 'Transaction latch (onComplete) measures the actual present time; a stable lateness gradually allows earlier frame timelines (up to +2.5 ms), and the slack is released when there is headroom. Needs “Release by onComplete” and Android 13+ (timelines)',
  dyn_ptshint: 'Per the log some frames arrive without a timestamp (PTS): those keep the arrival-based phase, as before.',
  dyn_mehint: 'Per the log hardware ME is unavailable (no extension or a GL error): DIS computes the flow.',
  dyn_thermhint: 'Per the log there is no battery temperature sensor — throttling is idle, the output always runs at full rate.',
  k_dup_thresh: 'Duplicate threshold', d_dup_thresh: 'How different frames must be to count as new: “Exact” skips identical frames only, “Soft” also nearly identical ones',
  k_panel_hz: 'Panel rate', d_panel_hz: 'SurfaceFlinger hint for the overlay and the game window; 0 = leave alone. Quick values are your display modes',
  k_panel_game_vote: 'Vote on the game window', d_panel_game_vote: 'Set the same rate on the game window (ANativeWindow_setFrameRate)',
  k_panel_anchor: 'Anchor layer', d_panel_anchor: 'A separate transparent layer updated at the panel rate keeps the mode even while the overlay is idle',
  k_use_oncomplete: 'Release via onComplete', d_use_oncomplete: 'A buffer is free once SurfaceFlinger returned it; off = by “Buffer hold”',
  k_layer_tf: 'Layer rotation', d_layer_tf: 'If the picture appears rotated or mirrored (pre-rotating games) pick an angle or a flip. “From game” is the default',
  k_elect_blast: 'BLASTBufferQueue hints', d_elect_blast: 'Window sizes from BLASTBufferQueue help pick the game queue (hook at launch)',
  k_hook_bbq: 'Video window via BLAST', d_hook_bbq: 'Video and frames from a GPU process (Binder): the overlay layer attaches exactly to the SurfaceView and stays under the app UI. Off - such sources are ignored',
  k_ui_source: 'App window as source', d_ui_source: 'Allow capturing the app UI window (HWUI). Usually not wanted: it covers video with a black rectangle',
  k_hook_swap: 'Fallback capture: eglSwapBuffers', d_hook_swap: 'GLES games when BufferQueue yields no frames. “Fallback” arms after the delay below; “Always” installs the hook at once. Hook at launch',
  k_hook_present: 'Fallback capture: vkQueuePresentKHR', d_hook_present: 'Vulkan games. The vkCreateDevice hook must be in place before the device exists: “Fallback” = only if the BufferQueue hooks failed, “Always” = always. Applied at launch',
  k_fallback_ms: 'Arm fallback capture after', d_fallback_ms: 'Milliseconds without BufferQueue frames after launch; the fallback stays silent while BufferQueue works',
  k_queue_prio: 'High queue priority', d_queue_prio: 'Ask the driver for a high priority queue for the module; falls back to normal if refused. Applied at launch',
  k_live: 'Apply without restart', d_live: 'The game picks up safe settings right after saving (hooks and queue only at launch)',
  advNote: 'Change only while diagnosing. hook_vk can hang the game at launch.',
  m_title: 'Generation mode', m_desc: 'How many frames to add between the game\'s frames',
  m_auto: 'Auto', m_mult: 'Multiplier', m_target: 'Target FPS',
  s_auto: () => 'Output follows the display rate: the lower the game FPS, the more frames are added.',
  s_mult: n => `×${n}: ${n - 1} frame(s) inserted between game frames — output ${n}× as often, but never above the display rate.`,
  s_target: (f, g) => `Output is pulled up to ${f} FPS; the number of added frames is chosen automatically, at most ${g} per game frame.`,
  s_off: () => 'Max generated frames = 0: nothing is added, the module only passes the picture through.',
  p_title: 'Quality & speed', p_desc: 'Quick choice of flow quality and GPU budget. “Custom” opens the manual sliders.',
  p_perf: 'Speed', p_bal: 'Balanced', p_qual: 'Quality', p_custom: 'Custom',
  z_srgb: 'sRGB', z_auto: 'auto', z_off: 'off', z_never: 'never', z_game: 'from game', z_unset: 'none',
  resetOne: 'Restore the global value', resetCard: 'Reset this group to defaults', confirmReset: 'Reset?', resetDone: 'Reset',
  autoRefresh: 'Auto', mIdle: 'game sends no frames', mGpu: 'GPU', mGov: 'load reduction', mWarn: 'warnings', mHdr: 'HDR',
  k_debug: 'Verbose log', d_debug: 'Addresses, formats, first frames; stats once per second. Use only for diagnostics',
  g_hud: 'FPS counter',
  hm_fps: 'FPS', hm_arrow: 'Game→output', hm_full: 'Detailed', hp_tl: '↖', hp_tr: '↗', hp_bl: '↙', hp_br: '↘',
  k_hud: 'FPS counter over the game', d_hud: 'A layer of its own on top of the game shows how many frames per second are really output and how many of them the module added',
  k_hud_mode: 'What to show', d_hud_mode: 'FPS = output rate; Game→output = “30>120”; Detailed = FPS, GAME (game frames), GEN (added by the module), GPU (pass time)',
  k_hud_pos: 'Screen corner', d_hud_pos: 'Where the counter sits',
  k_hud_scale: 'Size', d_hud_scale: 'Text scale, 1…8',
  k_hud_tf: 'Counter rotation', d_hud_tf: 'If the counter appears rotated, pick an angle. Usually 0°',
  licenseHint: 'This module is distributed under the GNU GPL version 3. The full text is in the LICENSE file in the module directory.',
  licenseMissing: 'LICENSE file not found. Text: https://www.gnu.org/licenses/gpl-3.0.txt',
  g_general: 'Module', g_gen: 'Frame generation', g_out: 'Buffer release', g_cap: 'Capture', g_adv: 'Hooks & diagnostics',
  k_enable: 'Frame generation', d_enable: 'Master switch (in a game profile — for that game only)',
  k_max_gen: 'Max generated frames', d_max_gen: 'Cap per game frame in Auto and Target FPS modes: 0 = pass-through, up to 7. Not used in Multiplier mode',
  k_target_fps: 'Target FPS', d_target_fps: 'Output rate to aim for; the display cannot show more than its refresh rate',
  k_flow_side: 'Flow resolution', d_flow_side: 'Short side for optical flow: lower is faster, higher is more accurate',
  k_max_dt_ms: 'Max gap between frames', d_max_dt_ms: 'A single gap between frames longer than this skips interpolation for that frame (a hitch). Minimum 50 ms: lower values disable generation',
  k_min_fps: 'Min source FPS', d_min_fps: 'Below this game FPS generation is off. Values under 20 change nothing: WFG already turns off below 20 and back on above 24',
  k_zorder: 'Layer z-order', d_zorder: 'Overlay position relative to the game window',
  k_display_fps: 'Layer rate (fixed)', d_display_fps: 'SurfaceFlinger frame-rate hint; 0 = unset',
  k_auto_rate: 'Auto layer rate', d_auto_rate: 'Source FPS × (frames+1) when no fixed rate is set. Affects the panel mode',
  k_dataspace: 'Colour space', d_dataspace: 'Forward the game\'s dataspace to the overlay',
  k_hide_ms: 'Hide overlay without frames', d_hide_ms: 'Milliseconds without frames before the game layer shows; 0 = never',
  k_release_lag: 'Buffer hold', d_release_lag: 'How many presents to keep a frame after output (0–8)',
  k_acq_fence: 'Wait for frame ready', d_acq_fence: 'Wait on the game\'s acquire fence before copying',
  k_target_w: 'Game buffer width', d_target_w: 'Force queue election by size; 0 = auto',
  k_target_h: 'Game buffer height', d_target_h: 'Force queue election by size; 0 = auto',
  k_hook_surface: 'Surface hook', d_hook_surface: 'Window, buffer and fence from Surface::hook_queueBuffer (recommended)',
  k_hook_egl: 'Fallback: EGL', d_hook_egl: 'Window via eglCreateWindowSurface: “Fallback” = only if the Surface hook failed',
  k_hook_anw: 'Fallback: ANativeWindow', d_hook_anw: 'Window via ANativeWindow_fromSurface: “Fallback” = only if the Surface hook failed',
  k_hook_vk: 'Swapchain patch (Vulkan)', d_hook_vk: 'Dangerous: the libvulkan hook can hang the game at launch',
  k_hook_delay_ms: 'Hook install delay', d_hook_delay_ms: 'Pause after process start, ms (hang bisecting)',
  k_stats: 'Stats in logcat', d_stats: 'One line every 5 seconds while the game sends frames: logcat -s myfg',
  g_fine: 'Fine tuning',
  x_title: 'Expert settings', x_desc: 'Capture, hooks and diagnostics. Only needed if a game does not start or yields no frames', x_changed: 'changed',
  c_title: 'Compatibility mode', c_desc: 'If a game hangs on the splash screen try “Gentle”, then “Minimal”. Applied at the next game launch',
  c_std: 'Standard', c_soft: 'Gentle', c_min: 'Minimal',
  c_s_std: 'All hooks on, no delay.',
  c_s_soft: 'Hooks are installed 3 seconds after process start — avoids a race with the game threads at launch.',
  c_s_min: 'BufferQueue hooks only: no Surface hook and no window lookup via EGL/ANativeWindow.',
  c_s_custom: 'A custom combination of hooks and delay (settings below).',
  q_fromLog: 'Take from log', q_auto: 'Auto', q_set: 'Queue pinned', q_none: 'No elected queue in the log — launch the game and try again',
  q_note: 'Pins the game queue size from the last run (the “game queue elected” log line). “Auto” lets the module choose.',
  a_plan: (s, o, g) => `From the log: game ≈${s} → output ≈${o} fps, +${g} frame(s) per game frame.`,
  a_plan0: s => `From the log: game ≈${s} fps — no frames added (game FPS below the 20–24 gate, or output already at the display rate).`,
  a_stats: (s, o) => `From the log: game ≈${s} → output ≈${o} fps.`,
  gov_now: (l, f) => `The load governor lowered the flow resolution to ≈${f} px (level ${l}).`,
  gov_now0: l => `The load governor lowered the flow resolution (level ${l}).`,
  dyn_hdrhint: 'Per the log the panel reports no HDR support, so the output stays SDR. If the device really supports HDR, enable HDR in the system display settings.',
  dyn_duphint: 'Per the log duplicate detection is unavailable: the game buffer has an external (YCbCr) format.',
  pk_exact: 'Exact', pk_normal: 'Normal', pk_soft: 'Soft',
},
};

let LANG = 'en';
try { LANG = localStorage.getItem('myfg_lang') || ''; } catch (e) { LANG = ''; }
if (LANG !== 'ru' && LANG !== 'en') LANG = (navigator.language || '').toLowerCase().startsWith('ru') ? 'ru' : 'en';
const t = k => (T[LANG] && T[LANG][k]) || T.en[k] || k;
const tf = (k, ...a) => { const f = (T[LANG] && T[LANG][k]) || T.en[k]; return typeof f === 'function' ? f(...a) : (f || k); };

/* ══════════════════════════════════════════════════════════════════════════
   Доступ к устройству
══════════════════════════════════════════════════════════════════════════ */
const q = s => `'${String(s).replace(/'/g, `'\\''`)}'`;
function toB64(s) {
  const bytes = new TextEncoder().encode(s);
  let bin = '';
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin);
}

const REAL = !!(window.ksu && typeof window.ksu.exec === 'function');

const real = {
  sh(cmd) {
    return new Promise(resolve => {
      const cb = `__myfg_cb_${Date.now()}_${Math.floor(Math.random() * 1e6)}`;
      window[cb] = (errno, out, err) => { delete window[cb]; resolve({ code: Number(errno), out: out || '', err: err || '' }); };
      try { window.ksu.exec(cmd, '{}', cb); }
      catch (e) { delete window[cb]; resolve({ code: -1, out: '', err: String(e) }); }
    });
  },
  async read(path) {
    const r = await this.sh(`cat ${q(path)} 2>/dev/null`);
    return r.code === 0 ? r.out : null;
  },
  // Запись через временный файл и mv: companion не увидит наполовину записанный конфиг.
  async write(path, text) {
    const dir = path.slice(0, path.lastIndexOf('/'));
    const r = await this.sh(
      `mkdir -p ${q(dir)} && printf '%s' ${q(toB64(text))} | base64 -d > ${q(path + '.tmp')}` +
      ` && mv -f ${q(path + '.tmp')} ${q(path)} && chmod 644 ${q(path)}`);
    if (r.code !== 0) throw new Error((r.err || 'exit ' + r.code).trim());
  },
  async list(dir) {
    const r = await this.sh(`ls -1 ${q(dir)} 2>/dev/null`);
    return r.code === 0 ? r.out.split('\n').map(s => s.trim()).filter(Boolean) : [];
  },
  async remove(path) { await this.sh(`rm -f ${q(path)}`); },
  async installed() {
    const r = await this.sh('pm list packages -3 2>/dev/null');
    return r.out.split('\n').map(s => s.trim().replace(/^package:/, '')).filter(s => PKG_RE.test(s)).sort();
  },
  // Лог модуля + какие из перечисленных игр сейчас запущены (pidof). Формат brief задан явно: в нём PID в
  // скобках после тега ("I/myfg    ( 1234): ..."), по нему лог делится между играми. Разбор — в scopeLog().
  async gameLog(cands) {
    const list = (cands || []).filter(p => PKG_RE.test(p));
    const runs = list.length
      ? `for p in ${list.map(q).join(' ')}; do pid=$(pidof -s "$p" 2>/dev/null); [ -n "$pid" ] && echo "@@RUN $p $pid"; done; `
      : '';
    const r = await this.sh(`${runs}logcat -d -v brief -s myfg WFG 2>/dev/null | tail -n 500`);
    const out = [], found = [];
    for (const l of r.out.split('\n')) {
      const m = l.match(/^@@RUN (\S+) (\d+)\s*$/);
      if (m) found.push({ pkg: m[1], pid: m[2] }); else out.push(l);
    }
    return { text: out.join('\n'), runs: found };
  },
  async forceStop(pkg) { return this.sh(`am force-stop ${q(pkg)}`); },
  // Отправить настройки в уже запущенные игры (root-скрипт модуля).
  async pushLive() { return this.sh(`sh ${q(LIVE)}`); },
  async moduleProp() { return this.read(PROP); },
  // Режимы экрана (Гц) из dumpsys display. Формат вывода зависит от версии Android: берём все «fps=NN.N».
  async displayRates() {
    const r = await this.sh("dumpsys display 2>/dev/null | grep -oE 'fps=[0-9]+(\\.[0-9]+)?' | sort -u");
    return normRates(r.out.split('\n').map(x => x.replace('fps=', '')));
  },
};
const normRates = list => [...new Set(list.map(x => Math.round(Number(x))).filter(n => n >= 24 && n <= 480))].sort((a, b) => a - b);

// Демо-режим (страница открыта не в менеджере): файловая система в памяти.
const mockFs = new Map([
  [`${CFG_DIR}/packages.list`, '# MyFG games\ncom.example.racer\ncom.example.shooter\n'],
  [`${CFG_DIR}/config`, 'enable=1\nmax_gen=3\ntarget_fps=0\nflow_side=180\nhide_ms=300\n'],
  [`${CFG_DIR}/config.d/com.example.racer`, 'target_fps=60\nmax_gen=1\n'],
]);
const mock = {
  async read(p) { return mockFs.has(p) ? mockFs.get(p) : null; },
  async write(p, text) { mockFs.set(p, text); },
  async list(d) { return [...mockFs.keys()].filter(k => k.startsWith(d + '/')).map(k => k.slice(d.length + 1)); },
  async remove(p) { mockFs.delete(p); },
  async installed() { return ['com.example.racer', 'com.example.shooter', 'org.example.puzzle', 'io.example.rpg']; },
  async gameLog(cands) {
    return { text: mockLogText(), runs: (cands || []).includes('com.example.racer') ? [{ pkg: 'com.example.racer', pid: '1234' }] : [] };
  },
  async forceStop() { return { code: 0, out: '', err: '' }; },
  async pushLive() { mockFs.set('__live_pushes', String(Number(mockFs.get('__live_pushes') || 0) + 1)); return { code: 0, out: '', err: '' }; },
  async moduleProp() { return 'id=myfg\nversion=v0.9\nversionCode=9\n'; },
  async displayRates() { return [60, 90, 120]; },
};
function mockLogText() {
  if (window.__mockLog != null) return window.__mockLog;
  {
    return [
      'I/myfg    ( 1234): com.example.racer: mode=x3 max_gen=2 flow=180px min_fps=15 adaptive=1(50%) hdr=0 panel=120Hz dup=0 live=1',
      'I/myfg    ( 1234): capture hooks installed: BufferQueue+attach+Surface',
      'I/myfg    ( 1234): capture: game queue elected 2000x1200 (31 frames/s)',
      'I/myfg    ( 1234): vk: device Adreno (TM) 840 (api 1.3.0) queue family 0, sync_fd=1, extFormats=1 ycbcr=1',
      'I/myfg    ( 1234): first game frame: 2000x1200 transform=0 dataspace=0x8c20000',
      'I/WFG     ( 1234): WFG plan: source 30.1 fps, target 120.0 fps, ratio 3.99 -> 3 generated (capacity 7, output 120.4 fps)',
      'I/myfg    ( 1234): overlay layer created on window 0x7a8c (2000x1200)',
      'I/myfg    ( 1234): pipeline up',
      'W/myfg    ( 1234): import of game buffer failed (usage/format?) x1',
      'W/myfg    ( 1234): dup_detect unavailable: the game buffer has an external (YCbCr) format',
      'I/myfg    ( 1234): src=30.0/s gen=90.0/s shown=120.0/s | gpu=4.2ms gov=1 | dropLate=2',
    ].join('\n');
  }
}
const B = REAL ? real : mock;

/* ══════════════════════════════════════════════════════════════════════════
   Состояние
══════════════════════════════════════════════════════════════════════════ */
let globalMap = new Map();     // все строки глобального config, включая неизвестные ключи
const pkgMaps = new Map();     // pkg -> Map переопределений (загружается при выборе профиля)
let pkgs = [];                 // packages.list
let pkgHeader = [];
let hasProfile = new Set();    // пакеты, у которых есть файл config.d/<pkg>
let scope = GLOBAL;
let customOpen = false;        // пользователь открыл ручные ползунки качества («Свой»)
let DISPLAY_RATES = [];        // режимы экрана, Гц
let EXPERT = false;
try { EXPERT = localStorage.getItem('myfg_expert') === '1'; } catch (e) { EXPERT = false; }
const panelQuick = () => (DISPLAY_RATES.length ? [0, ...DISPLAY_RATES] : [0, 60, 90, 120, 144]);

const $ = id => document.getElementById(id);
function h(tag, props, ...kids) {
  const e = document.createElement(tag);
  if (props) for (const [k, v] of Object.entries(props)) {
    if (k === 'class') e.className = v;
    else if (k === 'text') e.textContent = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat()) if (c != null) e.append(c);
  return e;
}

let toastTimer = null;
function toast(msg, isErr) {
  const el = $('toast');
  el.textContent = msg;
  el.classList.toggle('err', !!isErr);
  el.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('show'), isErr ? 3600 : 1400);
}

/* ── Разбор и запись файлов настроек ─────────────────────────────────────── */
function parseCfg(text) {
  const m = new Map();
  for (const line of (text || '').split('\n')) {
    const s = line.trim();
    if (!s || s[0] === '#') continue;
    const i = s.indexOf('=');
    if (i > 0) m.set(s.slice(0, i).trim(), s.slice(i + 1).trim());
  }
  return m;
}
const clampInt = (v, it) => {
  let n = Math.round(Number(v));
  if (!Number.isFinite(n)) n = it.def;
  if (it.t === 'num' || it.t === 'pick') n = Math.min(it.max, Math.max(it.min, n));
  else if (it.t === 'bool') n = n ? 1 : 0;
  else n = Math.min((it.opts ? it.opts.length : 3) - 1, Math.max(0, n));
  if (it.zsnap && n > 0 && n < it.zsnap) n = it.zsnap;      // между «авто» (0) и нижней границей значений нет
  return n;
};
function serializeGlobal() {
  const lines = ['# MyFG — global config (written by WebUI)'];
  for (const it of ALL_ITEMS) lines.push(`${it.k}=${clampInt(globalMap.has(it.k) ? globalMap.get(it.k) : it.def, it)}`);
  for (const [k, v] of globalMap) if (!BY_KEY[k]) lines.push(`${k}=${v}`);     // чужие ключи не теряем
  return lines.join('\n') + '\n';
}
function serializePkg(pkg) {
  const m = pkgMaps.get(pkg);
  if (!m || !m.size) return null;
  const lines = [`# MyFG — overrides for ${pkg} (written by WebUI)`];
  for (const it of ALL_ITEMS) if (m.has(it.k)) lines.push(`${it.k}=${clampInt(m.get(it.k), it)}`);
  for (const [k, v] of m) if (!BY_KEY[k]) lines.push(`${k}=${v}`);
  return lines.join('\n') + '\n';
}
function serializePkgs() {
  const head = pkgHeader.length ? pkgHeader : ['# MyFG — games, one package per line'];
  return [...head, ...pkgs].join('\n') + '\n';
}

let saveTimer = null;
const dirty = new Set();
function scheduleSave(target) {
  dirty.add(target);
  clearTimeout(saveTimer);
  saveTimer = setTimeout(flush, 350);
}
async function flush() {
  clearTimeout(saveTimer);
  const targets = [...dirty];
  dirty.clear();
  if (!targets.length) return true;
  try {
    for (const tg of targets) {
      if (tg === GLOBAL) await B.write(`${CFG_DIR}/config`, serializeGlobal());
      else {
        const text = serializePkg(tg);
        const path = `${CFG_DIR}/config.d/${tg}`;
        if (text == null) { await B.remove(path); hasProfile.delete(tg); }
        else { await B.write(path, text); hasProfile.add(tg); }
      }
    }
    try { await B.pushLive(); } catch (e) { /* игра не запущена или live.sh нет — применится при запуске */ }
    toast(t('saved'));
    renderPkgs();
    return true;
  } catch (e) {
    toast(`${t('saveFail')}: ${e.message}`, true);
    return false;
  }
}

/* ══════════════════════════════════════════════════════════════════════════
   Значения с учётом профиля
══════════════════════════════════════════════════════════════════════════ */
const curMap = () => (scope === GLOBAL ? globalMap : pkgMaps.get(scope));
function effective(k) {
  const it = BY_KEY[k];
  if (scope !== GLOBAL && pkgMaps.get(scope).has(k)) return clampInt(pkgMaps.get(scope).get(k), it);
  return clampInt(globalMap.has(k) ? globalMap.get(k) : it.def, it);
}
const overridden = k => scope !== GLOBAL && pkgMaps.get(scope).has(k);

function setVal(k, v) {
  curMap().set(k, String(clampInt(v, BY_KEY[k])));
  scheduleSave(scope);
  paintAll();
}
function setMany(o) {
  const m = curMap();
  for (const [k, v] of Object.entries(o)) m.set(k, String(clampInt(v, BY_KEY[k])));
  scheduleSave(scope);
  paintAll();
}
function resetVal(k) {
  if (scope === GLOBAL) return;
  pkgMaps.get(scope).delete(k);
  scheduleSave(scope);
  paintAll();
}

/* Условия «настройка имеет смысл»: список условий, достаточно одного. */
function depOk(it) {
  if (!it.dep) return true;
  return it.dep.some(([k, op, v]) => {
    const x = effective(k);
    return op === '=' ? x === v : op === '>' ? x > v : x !== v;
  });
}
const loadCustomVisible = () => customOpen || activePreset() === '';
function visibleNow(it) {
  if (it.show && ![].concat(it.show).includes(currentMode())) return false;
  if (it.vis === 'custom' && !loadCustomVisible()) return false;
  return true;
}
const currentMode = () => (effective('multiplier') >= 2 ? 'mult' : effective('target_fps') > 0 ? 'target' : 'auto');
let lastMult = 0, lastTarget = 0;      // последние значения режимов — чтобы переключение туда-обратно их не теряло
function setMode(m) {
  const mu = effective('multiplier'), tf = effective('target_fps');
  if (mu >= 2) lastMult = mu;
  if (tf > 0) lastTarget = tf;
  if (m === 'auto') setMany({ multiplier: 0, target_fps: 0 });
  else if (m === 'mult') setMany({ multiplier: lastMult || 2, target_fps: 0 });
  else setMany({ multiplier: 0, target_fps: lastTarget || 60 });
}
function activePreset() {
  for (const [name, p] of Object.entries(PRESETS))
    if (Object.entries(p).every(([k, v]) => effective(k) === v)) return name;
  return '';
}

/* ══════════════════════════════════════════════════════════════════════════
   Отрисовка настроек
══════════════════════════════════════════════════════════════════════════ */
let FOLD = {};
try { FOLD = JSON.parse(localStorage.getItem('myfg_fold') || '{}') || {}; } catch (e) { FOLD = {}; }
const saveFold = () => { try { localStorage.setItem('myfg_fold', JSON.stringify(FOLD)); } catch (e) { /* без хранилища тоже работает */ } };

function resetBtn(k) {
  return h('button', {
    class: 'tu-reset hidden', type: 'button', 'data-reset': k, text: '↺', title: t('resetOne'),
    onclick: e => { e.preventDefault(); e.stopPropagation(); resetVal(k); },
  });
}
const iconBox = ic => h('span', { class: 'tu-row-icon', text: ic });
// Ключи, которые игра читает только при запуске (хуки, очередь, создание устройства): в live-обновление
// не входят (Config::applyLive в config.cpp). Под такими настройками показывается пометка.
// target_w/target_h/elect_blast/ui_source читаются на каждом кадре и применяются на ходу (см. applyLive).
const RESTART_KEYS = new Set(['hook_surface', 'hook_egl', 'hook_anw', 'hook_vk', 'hook_swap', 'hook_present', 'hook_delay_ms',
  'fallback_ms', 'queue_prio', 'hook_bbq']);
const textBox = (label, desc) => {
  const dk = label && label.dataset ? label.dataset.i18n : '';
  const key = dk && dk.startsWith('k_') ? dk.slice(2) : '';
  return h('div', { class: 'tu-row-text' }, label, desc,
    key && RESTART_KEYS.has(key) ? h('p', { class: 'tu-row-desc tu-restart', 'data-i18n': 'restartNote' }) : null);
};

/* Кнопка − / + с повтором при удержании. */
function stepBtn(txt, dir, k, sc) {
  let delay = null, rep = null;
  const stop = () => { clearTimeout(delay); clearInterval(rep); delay = rep = null; };
  const go = () => {
    if (sc !== scope) return stop();
    const it = BY_KEY[k];
    let nv = effective(k) + dir * (it.step || 1);
    if (it.zsnap && nv > 0 && nv < it.zsnap) nv = dir > 0 ? it.zsnap : 0;   // 0 («авто») <-> нижняя граница
    setVal(k, nv);
  };
  return h('button', {
    class: 'tu-step', type: 'button', text: txt, 'aria-label': (dir < 0 ? '−' : '+') + ' ' + k,
    onpointerdown: e => { if (e.button) return; go(); stop(); delay = setTimeout(() => { rep = setInterval(go, 70); }, 420); },
    onpointerup: stop, onpointerleave: stop, onpointercancel: stop, oncontextmenu: e => e.preventDefault(),
    onclick: e => { if (e.detail === 0) go(); },          // клавиатура: Enter / Space
  });
}

function buildNum(it) {
  const k = it.k, sc = scope, d = it.div || 1;
  const commit = v => { if (sc === scope) setVal(k, v); };
  const inp = h('input', {
    type: 'number', inputmode: d > 1 ? 'decimal' : 'numeric', enterkeyhint: 'done', class: 'tu-number-input',
    min: it.min / d, max: it.max / d, step: (it.step || 1) / d, 'data-ctl': k,
    onchange: e => {
      commit(e.target.value === '' ? it.def : Math.round(Number(e.target.value) * d));
      if (sc === scope) { const v = effective(k); e.target.value = d > 1 ? (v / d).toFixed(1) : v; }   // поле может остаться в фокусе (Enter): показываем принятое значение
    },
  });
  const unit = h('span', { class: 'tu-unit', 'data-unit': k });
  let slider = null;
  if (it.sl) {
    const lo = it.smin != null ? it.smin : it.min, hi = it.smax != null ? it.smax : it.max;
    slider = h('input', {
      type: 'range', class: 'tu-slider', min: lo, max: hi, step: it.step || 1, 'data-sl': k, 'aria-label': k,
      oninput: e => {       // предпросмотр без записи: число и подпись обновляются на лету
        const v = Number(e.target.value);
        inp.value = d > 1 ? (v / d).toFixed(1) : v;
        unit.textContent = zeroText(it, v);
        e.target.style.setProperty('--p', pct(v, lo, hi));
      },
      onchange: e => commit(Number(e.target.value)),
    });
  }
  // со слайдером: [−] ━━●━━ [+] [число ед.];  без слайдера: [−] [число] [+] ед.
  const line = slider
    ? h('div', { class: 'tu-ctl-line' }, stepBtn('−', -1, k, sc), slider, stepBtn('+', 1, k, sc), h('div', { class: 'tu-field' }, inp, unit))
    : h('div', { class: 'tu-ctl-line plain' }, stepBtn('−', -1, k, sc), inp, stepBtn('+', 1, k, sc), unit);
  const row = h('div', { class: 'tu-vm-row tu-num', 'data-k': k },
    h('div', { class: 'tu-vm-top' }, iconBox(it.icon), textBox(h('p', { class: 'tu-row-label', 'data-i18n': 'k_' + k }), h('p', { class: 'tu-row-desc', 'data-i18n': 'd_' + k })), resetBtn(k)),
    line);
  const qs = typeof it.quick === 'function' ? it.quick() : it.quick;
  if (qs) row.append(h('div', { class: 'tu-quick' }, qs.map(v =>
    h('button', { class: 'tu-qbtn', type: 'button', 'data-q': v, onclick: () => commit(v),
      text: it.z && v === zeroOf(it) ? t(it.z) : (v + (it.unit && it.unit !== '×' ? ' ' + it.unit : '')) }))));
  return row;
}
const zeroOf = it => (it.zv != null ? it.zv : 0);
const zeroText = (it, v) => (it.z && v === zeroOf(it) ? t(it.z) : (it.unit || ''));
const pct = (v, lo, hi) => (hi > lo ? Math.max(0, Math.min(100, ((v - lo) / (hi - lo)) * 100)) : 0) + '%';

function buildRow(it) {
  const k = it.k;
  const sc = scope;      // строки пересобираются при смене профиля; поздний change старой строки не должен писать в новый
  if (it.t === 'bool') {
    const label = h('p', { class: 'tu-row-label', 'data-i18n': 'k_' + k });
    const desc  = h('p', { class: 'tu-row-desc',  'data-i18n': 'd_' + k });
    const cb = h('input', { type: 'checkbox', class: 'peer sr-only', 'data-ctl': k, id: 'c_' + k,
      onchange: e => { if (sc === scope) setVal(k, e.target.checked ? 1 : 0); } });
    // for=: иначе label активирует первую кнопку внутри (↺), а не переключатель
    return h('label', { class: 'tu-row', 'data-k': k, for: 'c_' + k },
      iconBox(it.icon), textBox(label, desc), resetBtn(k),
      h('div', { class: 'tu-toggle-wrap' }, cb, h('div', { class: 'tu-toggle' })));
  }
  if (it.t === 'num') return buildNum(it);
  // mode: значения 0..N; pick: произвольный набор значений vals
  const labels = it.opts || ['modeOff', 'modeFallback', 'modeAlways'];
  const vals = it.vals || labels.map((_, i) => i);
  const seg = h('div', { class: 'tu-seg' + (it.wrap ? ' wrap' : ''), role: 'group' },
    labels.map((key, i) =>
      h('button', { class: 'tu-btn', type: 'button', 'data-v': String(vals[i]), 'data-i18n': key, onclick: () => { if (sc === scope) setVal(k, vals[i]); } })));
  return h('div', { class: 'tu-vm-row', 'data-k': k },
    h('div', { class: 'tu-vm-top' }, iconBox(it.icon), textBox(h('p', { class: 'tu-row-label', 'data-i18n': 'k_' + k }), h('p', { class: 'tu-row-desc', 'data-i18n': 'd_' + k })), resetBtn(k)),
    seg);
}

/* Выбор режима генерации: Авто / Множитель / Цель FPS + пояснение, что получится. */
function buildModeBlock() {
  const sc = scope;
  const seg = h('div', { class: 'tu-seg', role: 'group', 'data-mode': '1' },
    [['auto', 'm_auto'], ['mult', 'm_mult'], ['target', 'm_target']].map(([m, key]) =>
      h('button', { class: 'tu-btn', type: 'button', 'data-m': m, 'data-i18n': key, onclick: () => { if (sc === scope) setMode(m); } })));
  return h('div', { class: 'tu-vm-row', 'data-custom': 'mode' },
    h('div', { class: 'tu-vm-top' }, iconBox('🎚️'),
      textBox(h('p', { class: 'tu-row-label', 'data-i18n': 'm_title' }), h('p', { class: 'tu-row-desc', 'data-i18n': 'm_desc' })),
      h('button', { class: 'tu-reset hidden', type: 'button', 'data-reset-mode': '1', text: '↺', title: t('resetOne'),
        onclick: e => { e.preventDefault(); e.stopPropagation(); if (scope === GLOBAL) return; ['multiplier', 'target_fps'].forEach(k => pkgMaps.get(scope).delete(k)); scheduleSave(scope); paintAll(); } })),
    seg,
    h('p', { class: 'tu-sum', 'data-sum': '1' }),
    h('p', { class: 'tu-sum hidden', 'data-actual': '1' }));
}

/* Режим совместимости: один выбор вместо пяти ключей хуков. */
const activeCompat = () => {
  for (const [n, p] of Object.entries(COMPAT)) if (Object.entries(p).every(([k, v]) => effective(k) === v)) return n;
  return '';
};
function buildCompatBlock() {
  const sc = scope;
  const seg = h('div', { class: 'tu-seg', role: 'group', 'data-compat': '1' },
    [['std', 'c_std'], ['soft', 'c_soft'], ['min', 'c_min']].map(([m, key]) =>
      h('button', { class: 'tu-btn', type: 'button', 'data-c': m, 'data-i18n': key, onclick: () => { if (sc === scope) setMany(COMPAT[m]); } })));
  return h('div', { class: 'tu-vm-row', 'data-custom': 'compat' },
    h('div', { class: 'tu-vm-top' }, iconBox('🛡️'),
      textBox(h('p', { class: 'tu-row-label' }, h('span', { 'data-i18n': 'c_title' }), ' ', h('span', { class: 'tu-tag hidden', 'data-i18n': 'p_custom', 'data-tag': 'ccustom' })),
        h('p', { class: 'tu-row-desc', 'data-i18n': 'c_desc' }))),
    seg,
    h('p', { class: 'tu-sum', 'data-csum': '1' }));
}

/* Размер игровой очереди из лога вместо ручного ввода ширины и высоты. */
function buildQueueBlock() {
  const sc = scope;
  return h('div', { class: 'tu-vm-row', 'data-custom': 'queue' },
    h('div', { class: 'tu-controls wrap' },
      h('button', { class: 'tu-btn tu-btn-green', type: 'button', 'data-i18n': 'q_fromLog', onclick: async () => {
        if (sc !== scope) return;
        await loadLog(false);
        if (sc !== scope) return;
        if (INFO.queue) { setMany({ target_w: INFO.queue.w, target_h: INFO.queue.h }); toast(`${t('q_set')}: ${INFO.queue.w}×${INFO.queue.h}`); }
        else toast(t('q_none'), true);
      } }),
      h('button', { class: 'tu-btn', type: 'button', 'data-i18n': 'q_auto', onclick: () => { if (sc === scope) setMany({ target_w: 0, target_h: 0 }); } })),
    h('p', { class: 'tu-row-desc', style: 'margin-top:8px', 'data-i18n': 'q_note' }));
}

/* Переключатель экспертных настроек (хранится только в браузере менеджера). */
const expertKeys = () => SCHEMA.filter(g => g.expert).flatMap(g => g.items.map(i => i.k));
function expertChanged() {
  return expertKeys().filter(k => (scope === GLOBAL
    ? globalMap.has(k) && clampInt(globalMap.get(k), BY_KEY[k]) !== BY_KEY[k].def
    : overridden(k))).length;
}
function buildExpertSwitch() {
  const cb = h('input', { type: 'checkbox', class: 'peer sr-only', id: 'c_expert', 'data-ctl': 'expert',
    onchange: e => { EXPERT = e.target.checked; try { localStorage.setItem('myfg_expert', EXPERT ? '1' : '0'); } catch (err) { /* без хранилища тоже работает */ } paintExpert(); } });
  cb.checked = EXPERT;
  return h('section', { class: 'tu-card', 'data-g': 'expertsw' },
    h('label', { class: 'tu-row', for: 'c_expert' }, iconBox('🧰'),
      textBox(h('p', { class: 'tu-row-label' }, h('span', { 'data-i18n': 'x_title' }), ' ', h('span', { class: 'tu-tag hidden', 'data-tag': 'xchg' })),
        h('p', { class: 'tu-row-desc', 'data-i18n': 'x_desc' })),
      h('div', { class: 'tu-toggle-wrap' }, cb, h('div', { class: 'tu-toggle' }))));
}
function paintExpert() {
  document.querySelectorAll('[data-expert]').forEach(c => c.classList.toggle('hidden', !EXPERT));
  const cb = $('c_expert');
  if (cb) cb.checked = EXPERT;
  const n = expertChanged();
  const tag = document.querySelector('[data-tag="xchg"]');
  if (tag) { tag.textContent = n ? `${t('x_changed')}: ${n}` : ''; tag.classList.toggle('hidden', !n); }
}

/* Сведения из лога выбранной игры. Показываем «что получилось» только для запущенной игры: у остановленной
   (или с выключенной генерацией, или стоящей в меню) прежняя строка вводила бы в заблуждение. */
const INFO = { plan: null, stats: null, idle: false, paused: false, running: false, govFlow: 0, queue: null, dupUnavail: false, hdrNo: false, ptsPartial: false, meOff: false, thermNo: false, thermCapped: false };
const LINE_PID = /^[VDIWEF]\/\S*\s*\(\s*(\d+)\)/;
const linePid = l => { const m = LINE_PID.exec(l); return m ? m[1] : null; };

/* Лог одной игры. runs — запущенные сейчас игры из списка (pidof). Для выбранной игры берём строки её процесса:
   запущена — по текущему PID, нет — по PID последнего запуска (находим по строке старта «<пакет>: …»).
   Для «Глобально» — первая запущенная игра, иначе хвост общего лога без разбора по играм. */
function scopeLog(res, sc) {
  const lines = (res.text || '').split('\n').filter(l => l.trim());
  const runs = res.runs || [];
  let pid = null, pkg = '', running = false;
  if (sc !== GLOBAL) {
    pkg = sc;
    const r = runs.find(x => x.pkg === sc);
    if (r) { pid = r.pid; running = true; }
    else for (let i = lines.length - 1; i >= 0 && !pid; i--) if (lines[i].includes(sc + ': ')) pid = linePid(lines[i]);
  } else if (runs.length) { pid = runs[0].pid; pkg = runs[0].pkg; running = true; }
  const picked = pid ? lines.filter(l => linePid(l) === pid) : (sc === GLOBAL ? lines : []);
  return { text: picked.slice(-160).join('\n'), running, pkg, pid };
}

function parseInfo(text, running) {
  INFO.plan = INFO.stats = INFO.queue = null; INFO.idle = INFO.paused = false; INFO.govFlow = 0; INFO.dupUnavail = false; INFO.hdrNo = false; INFO.ptsPartial = INFO.meOff = false; INFO.thermNo = INFO.thermCapped = false;
  INFO.running = !!running;
  const lines = (text || '').split('\n');
  let decided = false, govSeen = false;
  for (let i = lines.length - 1; i >= 0; i--) {
    const l = lines[i];
    if (!decided) {
      const m = l.match(/src=([\d.]+)\/s gen=([\d.]+)\/s shown=([\d.]+)\/s/);
      if (m) { decided = true; INFO.stats = { src: +m[1], gen: +m[2], shown: +m[3], gov: +((l.match(/gov=(\d+)/) || [0, 0])[1]), hdr: /\bhdr (10bit|8bit)/.test(l) }; }
      else if (/idle: no frames/.test(l)) { decided = true; INFO.idle = true; }
      else if (/frame generation paused/.test(l)) { decided = true; INFO.paused = true; }
      else if (/frame generation resumed/.test(l)) decided = true;
    }
    if (!INFO.plan) {
      const p = l.match(/WFG plan: source ([\d.]+) fps, target ([\d.]+) fps, ratio [\d.]+ -> (\d+) generated/);
      if (p) INFO.plan = { src: +p[1], target: +p[2], gen: +p[3] };
    }
    if (!govSeen) { const g = l.match(/governor: level \d+.*flow_side=(\d+)/); if (g) { govSeen = true; INFO.govFlow = +g[1]; } }
    if (!INFO.queue) { const q = l.match(/game queue elected (\d+)x(\d+)/); if (q) INFO.queue = { w: +q[1], h: +q[2] }; }
    if (/dup_detect unavailable/.test(l)) INFO.dupUnavail = true;
    if (/HDR: the panel reports no HDR support/.test(l)) INFO.hdrNo = true;
    if (/pts-phase: .*some frames carry no PTS/.test(l)) INFO.ptsPartial = true;
    if (/qcom-me: .*(DIS stays the flow solver)/.test(l)) INFO.meOff = true;
    if (/thermal: no battery temp sensor/.test(l)) INFO.thermNo = true;
    if (/thermal: battery .*C >= \d+C, output capped/.test(l)) INFO.thermCapped = true;
    if (/thermal: .*full rate restored/.test(l)) INFO.thermCapped = false;
  }
  // «Текущие» сведения (темп, нагрузка) актуальны только пока игра идёт и генерация включена.
  if (!INFO.running || INFO.paused || INFO.idle) { INFO.plan = null; INFO.stats = null; INFO.govFlow = 0; }
}
function actualText() {
  if (INFO.plan) {
    const p = INFO.plan, s = Math.round(p.src);
    return p.gen > 0 ? tf('a_plan', s, Math.round(p.src * (p.gen + 1)), p.gen) : tf('a_plan0', s);
  }
  if (INFO.stats) return tf('a_stats', Math.round(INFO.stats.src), Math.round(INFO.stats.shown));
  return '';
}

function buildPresetBlock() {
  const sc = scope;
  const seg = h('div', { class: 'tu-seg', role: 'group', 'data-preset': '1' },
    [['perf', 'p_perf'], ['bal', 'p_bal'], ['qual', 'p_qual'], ['custom', 'p_custom']].map(([p, key]) =>
      h('button', { class: 'tu-btn', type: 'button', 'data-p': p, 'data-i18n': key, onclick: () => {
        if (sc !== scope) return;
        if (p === 'custom') { customOpen = true; paintAll(); }
        else { customOpen = false; setMany(PRESETS[p]); }
      } })));
  return h('div', { class: 'tu-vm-row', 'data-custom': 'preset' },
    h('div', { class: 'tu-vm-top' }, iconBox('🎛️'),
      textBox(h('p', { class: 'tu-row-label', 'data-i18n': 'p_title' }), h('p', { class: 'tu-row-desc', 'data-i18n': 'p_desc' }))),
    seg,
    h('p', { class: 'tu-sum warn hidden', 'data-gov': '1' }));
}

function groupChanged(g) {
  return g.items.some(it => scope === GLOBAL
    ? (globalMap.has(it.k) && clampInt(globalMap.get(it.k), it) !== it.def)
    : overridden(it.k));
}
function resetGroup(g) {
  const m = curMap();
  g.items.forEach(it => m.delete(it.k));
  scheduleSave(scope);
  paintAll();
  toast(t('resetDone'));
}

function renderSettings() {
  const root = $('settings');
  root.textContent = '';
  let expertSw = false;
  for (const g of SCHEMA) {
    if (g.expert && !expertSw) { root.append(buildExpertSwitch()); expertSw = true; }
    const folded = g.id in FOLD ? !!FOLD[g.id] : !!g.adv;
    const rbtn = h('button', { class: 'tu-card-reset hidden', type: 'button', 'data-greset': g.id, text: '↺', title: t('resetCard') });
    let armed = null;
    rbtn.addEventListener('click', e => {
      e.stopPropagation();
      if (!armed) {
        rbtn.textContent = t('confirmReset'); rbtn.classList.add('armed');
        armed = setTimeout(() => { armed = null; rbtn.textContent = '↺'; rbtn.classList.remove('armed'); }, 3000);
        return;
      }
      clearTimeout(armed); armed = null; rbtn.textContent = '↺'; rbtn.classList.remove('armed');
      resetGroup(g);
    });
    const head = h('div', { class: 'tu-card-header', role: 'button', tabindex: 0, 'aria-expanded': String(!folded) },
      h('span', { class: 'tu-card-icon', text: g.icon }),
      h('h2', { class: 'tu-card-title', 'data-i18n': g.title }),
      rbtn,
      h('span', { class: 'tu-chev', 'aria-hidden': 'true', text: '▾' }));
    const card = h('section', { class: 'tu-card' + (folded ? ' fold' : ''), 'data-g': g.id }, head);
    const toggle = () => {
      const f = card.classList.toggle('fold');
      head.setAttribute('aria-expanded', String(!f));
      FOLD[g.id] = f; saveFold();
    };
    head.addEventListener('click', toggle);
    head.addEventListener('keydown', e => { if ((e.key === 'Enter' || e.key === ' ') && e.target === head) { e.preventDefault(); toggle(); } });
    if (g.custom === 'mode') card.append(buildModeBlock());
    if (g.custom === 'preset') card.append(buildPresetBlock());
    if (g.custom === 'compat') card.append(buildCompatBlock());
    if (g.expert) card.setAttribute('data-expert', '1');
    g.items.forEach(it => {
      card.append(buildRow(it));
      if (it.dyn) card.append(h('p', { class: 'tu-card-note warn hidden', 'data-dyn': it.dyn, 'data-i18n': 'dyn_' + it.dyn }));
    });
    if (g.after === 'queue') card.append(buildQueueBlock());
    if (g.note) card.append(h('p', { class: 'tu-card-note warn', 'data-i18n': g.note }));
    root.append(card);
  }
  applyI18n(root);
  paintAll();
}

function paintRow(k) {
  const it = BY_KEY[k];
  const row = document.querySelector(`[data-k="${k}"]`);
  if (!row) return;
  const v = effective(k);
  const ovr = overridden(k);
  row.classList.toggle('ovr', ovr);
  row.querySelector('[data-reset]').classList.toggle('hidden', !ovr);
  if (it.show || it.vis) row.classList.toggle('hidden', !visibleNow(it));
  const ok = depOk(it);
  row.classList.toggle('dis', !ok);
  row.querySelectorAll('input, button:not([data-reset])').forEach(c => { c.disabled = !ok; });
  if (it.t === 'bool') row.querySelector('[data-ctl]').checked = !!v;
  else if (it.t === 'num') {
    const d = it.div || 1;
    const i = row.querySelector('input.tu-number-input');
    if (document.activeElement !== i) i.value = d > 1 ? (v / d).toFixed(1) : v;
    const u = row.querySelector('[data-unit]');
    u.textContent = zeroText(it, v);
    u.classList.toggle('z', !!it.z && v === zeroOf(it));
    const sl = row.querySelector('.tu-slider');
    if (sl) { sl.value = v; sl.style.setProperty('--p', pct(v, Number(sl.min), Number(sl.max))); }
    row.querySelectorAll('[data-q]').forEach(b => b.classList.toggle('active', Number(b.dataset.q) === v));
    row.querySelectorAll('.tu-step').forEach((b, n) => { if (ok) b.disabled = n === 0 ? v <= it.min : v >= it.max; });
  } else row.querySelectorAll('[data-v]').forEach(b => b.classList.toggle('active', Number(b.dataset.v) === v));
}

function paintCustom() {
  const mode = currentMode();
  document.querySelectorAll('[data-mode] [data-m]').forEach(b => b.classList.toggle('active', b.dataset.m === mode));
  const mrow = document.querySelector('[data-custom="mode"]');
  if (mrow) {
    const o = overridden('multiplier') || overridden('target_fps');
    mrow.classList.toggle('ovr', o);
    mrow.querySelector('[data-reset-mode]').classList.toggle('hidden', !o);
    const mg = effective('max_gen');
    const sum = mrow.querySelector('[data-sum]');
    const off = mode !== 'mult' && mg === 0;       // в режиме «Множитель» max_gen не используется
    sum.classList.toggle('warn', off);
    if (off) sum.textContent = tf('s_off');
    else if (mode === 'mult') sum.textContent = tf('s_mult', effective('multiplier'));
    else if (mode === 'target') sum.textContent = tf('s_target', effective('target_fps'), mg);
    else sum.textContent = tf('s_auto');
    const act = mrow.querySelector('[data-actual]');
    const at = actualText();
    act.textContent = at;
    act.classList.toggle('hidden', !at);
  }
  const ap = activePreset(), cv = loadCustomVisible(), shown = customOpen ? '' : ap;
  document.querySelectorAll('[data-preset] [data-p]').forEach(b =>
    b.classList.toggle('active', b.dataset.p === 'custom' ? cv : b.dataset.p === shown));
  const gv = document.querySelector('[data-gov]');
  if (gv) {
    const on = effective('adaptive') === 1 && !!INFO.stats && INFO.stats.gov > 0;
    gv.classList.toggle('hidden', !on);
    if (on) gv.textContent = INFO.govFlow ? tf('gov_now', INFO.stats.gov, INFO.govFlow) : tf('gov_now0', INFO.stats.gov);
  }
  const ac = activeCompat();
  document.querySelectorAll('[data-compat] [data-c]').forEach(b => b.classList.toggle('active', b.dataset.c === ac));
  const ct = document.querySelector('[data-tag="ccustom"]');
  if (ct) ct.classList.toggle('hidden', !!ac);
  const cs = document.querySelector('[data-csum]');
  if (cs) cs.textContent = t('c_s_' + (ac || 'custom'));
  const dyn = {
    hdrhint: () => effective('hdr') === 1 && INFO.hdrNo,
    duphint: () => effective('dup_detect') === 1 && INFO.dupUnavail,
    ptshint: () => effective('pts_phase') === 1 && INFO.ptsPartial,
    mehint: () => effective('qcom_me') === 1 && INFO.meOff,
    thermhint: () => effective('thermal') === 1 && INFO.thermNo,
  };
  document.querySelectorAll('[data-dyn]').forEach(e => e.classList.toggle('hidden', !(dyn[e.dataset.dyn] && dyn[e.dataset.dyn]())));
  for (const g of SCHEMA) {
    const b = document.querySelector(`[data-greset="${g.id}"]`);
    if (b) b.classList.toggle('hidden', !groupChanged(g));
  }
  paintExpert();
}

function paintAll() {
  ALL_ITEMS.forEach(it => paintRow(it.k));
  paintCustom();
  paintBadge();
  document.body.classList.toggle('fg-off', effective('enable') === 0);
}

function paintBadge() {
  const on = clampInt(globalMap.has('enable') ? globalMap.get('enable') : 1, BY_KEY.enable) === 1;     // бейдж — про модуль в целом
  $('statusBadge').classList.toggle('off', !on);
  $('statusLabel').textContent = on ? t('on') : t('off');
}

/* ══════════════════════════════════════════════════════════════════════════
   Профили и список игр
══════════════════════════════════════════════════════════════════════════ */
async function loadProfile(pkg) {
  if (pkgMaps.has(pkg)) return;
  pkgMaps.set(pkg, parseCfg(await B.read(`${CFG_DIR}/config.d/${pkg}`)));
}

async function selectScope(next, scrollTop) {
  if (next !== GLOBAL && !pkgs.includes(next)) next = GLOBAL;
  await flush();
  if (next !== GLOBAL) await loadProfile(next);
  scope = next;
  customOpen = false;
  renderScopeSel();
  renderSettings();
  renderPkgs();
  const isPkg = scope !== GLOBAL;
  $('restartBtn').classList.toggle('hidden', !isPkg);
  $('scopeNote').textContent = t(isPkg ? 'notePkg' : 'noteGlobal');
  if (scrollTop) window.scrollTo({ top: 0, behavior: 'smooth' });
  loadLog($('logBox').dataset.live === '1');         // метрики и «по логу» — для выбранной игры, а не для прошлой
}

function renderScopeSel() {
  const sel = $('scopeSel');
  sel.textContent = '';
  sel.append(h('option', { value: GLOBAL, text: t('scopeGlobal') }));
  pkgs.forEach(p => sel.append(h('option', { value: p, text: p })));
  sel.value = scope;
}

let pendingDel = null, pendingDelTimer = null;
function renderPkgs() {
  const box = $('pkgList');
  box.textContent = '';
  if (!pkgs.length) { box.append(h('div', { class: 'tu-empty', text: t('noGames') })); return; }
  for (const p of pkgs) {
    const has = hasProfile.has(p);
    const del = h('button', {
      class: 'tu-btn tu-btn-danger tu-pkg-del', type: 'button', text: pendingDel === p ? t('del') : '✕',
      onclick: e => { e.stopPropagation(); onDelete(p); },
    });
    if (pendingDel === p) { del.style.width = 'auto'; del.style.padding = '0 10px'; }
    box.append(h('div', { class: 'tu-pkg-row' + (scope === p ? ' sel' : ''), onclick: () => selectScope(p, true) },
      h('span', { class: 'tu-row-icon', text: '🎮' }),
      h('div', { class: 'tu-row-text' },
        h('p', { class: 'tu-pkg-name', text: p }),
        h('p', { class: 'tu-pkg-sub' + (has ? ' has' : ''), text: has ? t('profileYes') : t('profileNo') })),
      del));
  }
}

async function onDelete(p) {
  if (pendingDel !== p) {
    pendingDel = p;
    clearTimeout(pendingDelTimer);
    pendingDelTimer = setTimeout(() => { pendingDel = null; renderPkgs(); }, 3000);
    renderPkgs();
    return;
  }
  pendingDel = null;
  pkgs = pkgs.filter(x => x !== p);
  try { await B.write(`${CFG_DIR}/packages.list`, serializePkgs()); toast(t('pkgRemoved')); }
  catch (e) { toast(`${t('saveFail')}: ${e.message}`, true); }
  if (scope === p) await selectScope(GLOBAL);
  else { renderScopeSel(); renderPkgs(); }
}

async function addPackages(list) {
  let added = 0;
  for (const raw of list) {
    const p = raw.trim();
    if (!PKG_RE.test(p)) { toast(t('badPkg') + ': ' + p, true); continue; }
    if (pkgs.includes(p)) { if (list.length === 1) toast(t('dup'), true); continue; }
    pkgs.push(p); added++;
  }
  if (!added) return false;
  try { await B.write(`${CFG_DIR}/packages.list`, serializePkgs()); toast(`${t('pkgAdded')}: ${added}`); }
  catch (e) { toast(`${t('saveFail')}: ${e.message}`, true); }
  renderScopeSel();
  renderPkgs();
  return true;
}

/* ── Лицензия ────────────────────────────────────────────────────────────── */
let licenseCache = null;
async function loadLicense() {
  if (licenseCache) return licenseCache;
  let txt = null;
  if (REAL) txt = await B.read(LICENSE_PATH);                 // файл LICENSE в каталоге модуля
  if (!txt) { try { const r = await fetch('license.txt'); if (r.ok) txt = await r.text(); } catch (e) { /* ниже запасной текст */ } }
  licenseCache = txt && txt.trim() ? reflow(txt) : t('licenseMissing');
  return licenseCache;
}
/* В файле строки жёстко перенесены по 80 колонкам — на узком экране это даёт рваный текст.
   Склеиваем строки одного абзаца: строка с отступом начинает новый абзац, строка без отступа продолжает предыдущий. */
function reflow(txt) {
  const out = [];
  let cur = null;
  for (const raw of txt.replace(/\r/g, '').split('\n')) {
    if (!raw.trim()) { if (cur !== null) out.push(cur); cur = null; out.push(''); continue; }
    const indent = raw.length - raw.trimStart().length;
    const line = indent > 12 ? raw.trim() : raw.replace(/\s+$/, '');      // центрированные заголовки — без отступа
    if (cur !== null && indent === 0) cur += ' ' + line;
    else { if (cur !== null) out.push(cur); cur = line; }
  }
  if (cur !== null) out.push(cur);
  return out.join('\n').replace(/\n{3,}/g, '\n\n');
}
async function openLicense() {
  $('licenseModal').classList.remove('hidden');
  const box = $('licenseText');
  box.textContent = t('loading');
  box.textContent = await loadLicense();
  box.scrollTop = 0;
}

/* ── Выбор из установленных ──────────────────────────────────────────────── */
let installed = [];
const picked = new Set();
function renderPicker() {
  const box = $('pickList');
  const f = $('pickSearch').value.trim().toLowerCase();
  box.textContent = '';
  const rows = installed.filter(p => !f || p.toLowerCase().includes(f));
  if (!rows.length) { box.append(h('div', { class: 'tu-file-empty', text: t('nothing') })); return; }
  for (const p of rows) {
    const done = pkgs.includes(p);
    const cb = h('input', { type: 'checkbox', class: 'tu-check', disabled: done, checked: done || picked.has(p),
      onchange: e => { e.target.checked ? picked.add(p) : picked.delete(p); } });
    box.append(h('label', { class: 'tu-file-row' + (done ? ' done' : '') }, cb,
      h('span', { class: 'tu-file-name', text: p }),
      done ? h('span', { class: 'tu-file-tag', text: t('added') }) : null));
  }
}
async function openPicker() {
  picked.clear();
  $('pickSearch').value = '';
  $('pickerModal').classList.remove('hidden');
  $('pickList').textContent = '';
  installed = await B.installed();
  renderPicker();
}

/* ══════════════════════════════════════════════════════════════════════════
   Лог и метрики
══════════════════════════════════════════════════════════════════════════ */
function renderChips(v) {
  const box = $('chips');
  box.textContent = '';
  const text = v ? v.text : '';
  const lines = (text || '').split('\n');
  const chip = (label, val, cls) => h('span', { class: 'tu-chip' + (cls ? ' ' + cls : '') }, label, val != null ? h('b', { text: val }) : null);
  // Последнее по времени событие: строка статистики, «нет кадров», пауза/возобновление
  let last = null;
  for (let i = lines.length - 1; i >= 0; i--) if (/src=[\d.]+\/s gen=|idle: no frames|frame generation (paused|resumed)/.test(lines[i])) { last = lines[i]; break; }
  if (!v || !v.running) box.append(chip(t('mStopped'), null, 'idle'));
  else if (last && /frame generation paused/.test(last)) box.append(chip(t('mPaused'), null, 'idle'));
  else if (last && /idle: no frames/.test(last)) box.append(chip(t('mIdle'), null, 'idle'));
  else if (last && /src=/.test(last)) {
    const m = last.match(/src=([\d.]+)\/s gen=([\d.]+)\/s shown=([\d.]+)\/s/);
    box.append(chip(t('mSrc'), m[1]), chip(t('mGen'), m[2]), chip(t('mShown'), m[3]));
    const gp = last.match(/gpu=([\d.]+)ms/);
    if (gp) box.append(chip(t('mGpu'), gp[1] + ' ms'));
    const gv = last.match(/gov=(\d+)/);
    if (gv) box.append(chip(t('mGov'), gv[1], 'bad'));
    if (/\bhdr (10bit|8bit)/.test(last)) box.append(chip(t('mHdr'), null, 'ok'));
    for (const key of ['dropLate', 'dropOut', 'dropReal', 'importFail', 'hidden']) {
      const mm = last.match(new RegExp(key + '=(\\d+)'));
      if (mm && Number(mm[1]) > 0) box.append(chip(t(key), mm[1], key !== 'dropLate' && key !== 'hidden' ? 'bad' : ''));
    }
  }
  const warns = lines.filter(l => /^[WE]\//.test(l)).length;
  if (warns) box.append(chip(t('mWarn'), String(warns), 'bad'));
}
function renderLog(text) {
  const box = $('logBox');
  box.textContent = '';
  if (!text) { box.textContent = t('logEmpty'); return; }
  for (const l of text.split('\n')) {
    const lv = l[0] === 'W' ? 'w' : l[0] === 'E' ? 'e' : '';
    box.append(h('span', { class: 'ln' + (lv ? ' ' + lv : ''), text: l + '\n' }));
  }
}
/* Строка над метриками: чей это лог и запущена ли игра. */
let LOGV = null;
function paintLogState() {
  const el = $('logState');
  if (!el) return;
  if (!LOGV) { el.textContent = ''; return; }
  const who = LOGV.pkg || (scope === GLOBAL ? '' : scope);
  el.textContent = LOGV.running ? tf('logRun', who) : tf('logStopped', who);
  el.classList.toggle('warn', !LOGV.running);
}
/* Читает лог выбранной игры, обновляет метрики, сведения «по логу» и подсказки; renderBox — перерисовать и сам лог. */
let logSeq = 0;
async function loadLog(renderBox) {
  const sc = scope, seq = ++logSeq;
  let res;
  try { res = await B.gameLog(sc === GLOBAL ? pkgs : [sc]); } catch (e) { res = { text: '', runs: [] }; }
  if (sc !== scope || seq !== logSeq) return;            // за время запроса выбрали другую игру / пришёл более свежий ответ
  const v = scopeLog(res, sc);
  LOGV = v;
  if (renderBox) {
    renderLog(v.text);
    $('logBox').dataset.live = '1';
    $('logBox').scrollTop = $('logBox').scrollHeight;
  }
  renderChips(v);
  parseInfo(v.text, v.running);
  paintLogState();
  paintCustom();
}
const refreshLog = () => loadLog(true);
let autoTimer = null;
function setAuto(on) {
  clearInterval(autoTimer); autoTimer = null;
  $('logAutoBtn').classList.toggle('active', on);
  $('logAutoBtn').setAttribute('aria-pressed', String(on));
  if (on) { refreshLog(); autoTimer = setInterval(() => { if (!document.hidden) refreshLog(); }, 2000); }
}
async function copyLog() {
  const text = $('logBox').textContent;
  try { await navigator.clipboard.writeText(text); }
  catch (e) {
    const ta = h('textarea', { style: 'position:fixed;opacity:0' });
    ta.value = text; document.body.append(ta); ta.select();
    try { document.execCommand('copy'); } catch (e2) { /* нечего делать */ }
    ta.remove();
  }
  toast(t('copied'));
}

/* ══════════════════════════════════════════════════════════════════════════
   i18n
══════════════════════════════════════════════════════════════════════════ */
function applyI18n(root) {
  (root || document).querySelectorAll('[data-i18n]').forEach(e => { e.textContent = t(e.dataset.i18n); });
  (root || document).querySelectorAll('[data-i18n-ph]').forEach(e => { e.placeholder = t(e.dataset.i18nPh); });
  (root || document).querySelectorAll('[data-i18n-aria]').forEach(e => { e.setAttribute('aria-label', t(e.dataset.i18nAria)); });
}
function setLang(l) {
  LANG = l;
  try { localStorage.setItem('myfg_lang', l); } catch (e) { /* без хранилища тоже работает */ }
  document.documentElement.lang = l;
  document.querySelectorAll('.tu-lang-btn').forEach(b => b.classList.toggle('active', b.dataset.lang === l));
  applyI18n();
  paintBadge();
  renderScopeSel();
  renderSettings();
  renderPkgs();
  if (LOGV) { renderChips(LOGV); paintLogState(); }
  $('scopeNote').textContent = t(scope === GLOBAL ? 'noteGlobal' : 'notePkg');
  if (!$('logBox').dataset.live) $('logBox').textContent = t('logEmpty');
  else refreshLog();
}

/* ══════════════════════════════════════════════════════════════════════════
   Старт
══════════════════════════════════════════════════════════════════════════ */
async function init() {
  document.querySelectorAll('.tu-lang-btn').forEach(b => b.addEventListener('click', () => setLang(b.dataset.lang)));
  $('scopeSel').addEventListener('change', e => selectScope(e.target.value));
  $('restartBtn').addEventListener('click', async () => {
    if (scope === GLOBAL) return;
    await flush();
    await B.forceStop(scope);
    toast(`${t('stopped')}: ${scope}`);
  });
  $('pkgAddBtn').addEventListener('click', async () => {
    const v = $('pkgInput').value.trim();
    if (!v) return;
    if (await addPackages([v])) $('pkgInput').value = '';
  });
  $('pkgInput').addEventListener('keydown', e => { if (e.key === 'Enter') $('pkgAddBtn').click(); });
  $('pkgPickBtn').addEventListener('click', openPicker);
  $('pickSearch').addEventListener('input', renderPicker);
  $('pickCloseBtn').addEventListener('click', () => $('pickerModal').classList.add('hidden'));
  $('pickApplyBtn').addEventListener('click', async () => {
    const list = [...picked];
    $('pickerModal').classList.add('hidden');
    if (list.length) await addPackages(list);
  });
  $('logRefreshBtn').addEventListener('click', refreshLog);
  $('logAutoBtn').addEventListener('click', () => setAuto(!autoTimer));
  $('logCopyBtn').addEventListener('click', copyLog);
  $('licenseBtn').addEventListener('click', openLicense);
  $('licenseCloseBtn').addEventListener('click', () => $('licenseModal').classList.add('hidden'));
  document.addEventListener('keydown', e => { if (e.key === 'Escape') $('licenseModal').classList.add('hidden'); });
  window.addEventListener('pagehide', () => { flush(); });
  document.addEventListener('visibilitychange', () => { if (document.hidden) flush(); else loadLog(false); });

  const [cfgText, pkgText, profileFiles, prop, rates] = await Promise.all([
    B.read(`${CFG_DIR}/config`), B.read(`${CFG_DIR}/packages.list`), B.list(`${CFG_DIR}/config.d`), B.moduleProp(),
    B.displayRates().catch(() => []),
  ]);
  DISPLAY_RATES = rates || [];
  globalMap = parseCfg(cfgText);
  pkgs = []; pkgHeader = [];
  for (const line of (pkgText || '').split('\n')) {
    const s = line.trim();
    if (!s) continue;
    if (s[0] === '#') { if (!pkgs.length) pkgHeader.push(s); continue; }
    if (PKG_RE.test(s) && !pkgs.includes(s)) pkgs.push(s);
  }
  hasProfile = new Set(profileFiles.filter(f => PKG_RE.test(f) && !f.endsWith('.tmp')));

  const pm = parseCfg(prop);
  $('moduleVer').textContent = (pm.get('version') || '?') + (REAL ? '' : ' · ' + t('preview'));

  setLang(LANG);
  await selectScope(GLOBAL);
  $('app').classList.remove('hidden');
  $('splash').remove();
  // Метрики и сведения «по логу» обновляются сами (раньше — только по кнопке): раз в 4 с, пока страница видна
  setInterval(() => { if (!document.hidden && !autoTimer) loadLog(false); }, 4000);
}

if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
else init();

// Для автотестов.
window.__myfg = { get state() { return { scope, pkgs: [...pkgs], global: Object.fromEntries(globalMap), expert: EXPERT, info: INFO, rates: DISPLAY_RATES }; }, flush, mockFs, B, scopeLog };
})();
