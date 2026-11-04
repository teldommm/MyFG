#!/system/bin/sh
# MyFG: отправляет текущие настройки в уже запущенные игры (без перезапуска).
# Вызывается WebUI после сохранения. Для каждой игры из packages.list кладёт собранный конфиг
# (глобальный + config.d/<pkg>) в /data/data/<pkg>/myfg_live с владельцем и меткой SELinux приложения.
# Игра раз в секунду смотрит на этот файл и применяет только безопасные на ходу поля.
MODDIR="${0%/*}"
CFG="$MODDIR/config"
[ -f "$CFG/packages.list" ] || exit 0
while IFS= read -r pkg || [ -n "$pkg" ]; do
  pkg=$(echo "$pkg" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
  case "$pkg" in ''|'#'*|.*|*[!A-Za-z0-9._]*) continue ;; esac
  d="/data/data/$pkg"
  [ -d "$d" ] || continue
  tmp="$d/.myfg_live.tmp"
  { cat "$CFG/config" 2>/dev/null; echo; cat "$CFG/config.d/$pkg" 2>/dev/null; } > "$tmp" 2>/dev/null || continue
  uid=$(stat -c %u "$d" 2>/dev/null)
  gid=$(stat -c %g "$d" 2>/dev/null)
  [ -n "$uid" ] && chown "$uid:$gid" "$tmp" 2>/dev/null
  chmod 600 "$tmp" 2>/dev/null
  chcon --reference="$d" "$tmp" 2>/dev/null
  mv -f "$tmp" "$d/myfg_live" 2>/dev/null
done < "$CFG/packages.list"
exit 0
