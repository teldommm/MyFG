#!/system/bin/sh
# MyFG: подготовка настроек. Выполняется от root при установке/обновлении.
# Настройки лежат В КАТАЛОГЕ МОДУЛЯ: /data/adb/modules/myfg/config/
#   packages.list   — игры (по пакету в строке)
#   config          — глобальные значения
#   config.d/<pkg>  — переопределения для одной игры
# Их пишет WebUI. При обновлении модуля каталог модуля заменяется целиком, поэтому здесь
# пользовательские настройки переносятся из прошлой версии в новую.
SKIPUNZIP=0
ID="${MODID:-myfg}"
CFG="$MODPATH/config"
OLD="/data/adb/modules/$ID/config"
LEGACY=/data/adb/myfg

if [ "$ARCH" != "arm64" ]; then
  abort "! Только arm64 (ARCH=$ARCH)"
fi

mkdir -p "$CFG/config.d"

if [ -d "$OLD" ]; then
  ui_print "- Перенос настроек из предыдущей версии"
  cp -rf "$OLD/." "$CFG/"
elif [ -d "$LEGACY" ]; then
  ui_print "- Перенос настроек из $LEGACY"
  cp -rf "$LEGACY/." "$CFG/"
fi

[ -f "$CFG/packages.list" ] || cat > "$CFG/packages.list" <<'EOT'
# По одному имени пакета в строке (удобнее через WebUI).
EOT
[ -f "$CFG/config" ] || cat > "$CFG/config" <<'EOT'
# key=value. Файл пишет WebUI; часть значений подхватывается сразу, остальные (хуки) — при следующем запуске игры.
enable=1
EOT

set_perm_recursive "$CFG" 0 0 0755 0644
[ -f "$MODPATH/live.sh" ] && set_perm "$MODPATH/live.sh" 0 0 0755
ui_print "- Настройки: $CFG"
ui_print "- Откройте WebUI модуля (менеджер -> MyFG), добавьте игры и настройте"
ui_print "- Логи: logcat -s myfg WFG"
