#!/usr/bin/env python3
"""
Достаёт из официального архива Android NDK (dl.google.com) ТОЛЬКО то, что нужно для сборки MyFG
родным clang из Termux: заголовки (libc/libc++/Vulkan/EGL/GLES/android), заглушки системных
библиотек для API 29, статическую libc++ и libunwind. Весь NDK (~650 МБ) не качается: читается
оглавление zip и запрашиваются нужные диапазоны байт (HTTP Range) - выходит ~30-40 МБ.

    fetch_ndk_sysroot.py <версия-NDK, напр. 27c> <куда-положить> [api=29]

Результат (мини-sysroot, совпадает по раскладке с NDK):
    <out>/usr/include/...                       заголовки
    <out>/usr/lib/aarch64-linux-android/        libc++_static.a libc++abi.a libunwind.a
    <out>/usr/lib/aarch64-linux-android/29/     crt*.o, libc.so, liblog.so, libandroid.so, ... (заглушки)
Только стандартная библиотека Python 3.
"""
import os
import struct
import sys
import time
import urllib.request
import zlib

NDK_URL = "https://dl.google.com/android/repository/android-ndk-r{ver}-linux.zip"
TRIPLE = "aarch64-linux-android"
GAP = 3 << 20          # соседние нужные файлы ближе 3 МБ читаем одним запросом
CHUNK_MAX = 64 << 20   # но не больше 64 МБ за запрос (память на телефоне)


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def http_range(url, start, end, tries=5):
    """Байты [start, end] (включительно); end=None - до конца. start<0 - хвост файла."""
    if start < 0:
        rng = "bytes=%d" % start
    else:
        rng = "bytes=%d-%s" % (start, "" if end is None else end)
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"Range": rng, "User-Agent": "myfg-build"})
            with urllib.request.urlopen(req, timeout=60) as r:
                if r.status != 206:
                    raise RuntimeError("сервер не поддерживает Range (HTTP %d)" % r.status)
                return r.read()
        except Exception as e:  # сеть на телефоне нестабильна - повторяем
            last = e
            time.sleep(1 + i * 2)
    raise RuntimeError("не удалось скачать %s [%s]: %s" % (url, rng, last))


def read_central_directory(url):
    tail = http_range(url, -(1 << 17), None)
    i = tail.rfind(b"PK\x05\x06")
    if i < 0:
        raise RuntimeError("не найден конец zip (EOCD)")
    n_total, cd_size, cd_off = struct.unpack("<HII", tail[i + 10:i + 20])
    if n_total == 0xFFFF or cd_off == 0xFFFFFFFF or cd_size == 0xFFFFFFFF:  # zip64
        j = tail.rfind(b"PK\x06\x07", 0, i)
        if j < 0:
            raise RuntimeError("zip64 locator не найден")
        z64_off = struct.unpack("<Q", tail[j + 8:j + 16])[0]
        rec = http_range(url, z64_off, z64_off + 55)
        n_total, cd_size, cd_off = struct.unpack("<QQQ", rec[32:56])
    cd = http_range(url, cd_off, cd_off + cd_size - 1)
    entries = []
    p = 0
    while p + 46 <= len(cd) and cd[p:p + 4] == b"PK\x01\x02":
        (_, _, _, flags, method, _, _, crc, csize, usize, nlen, elen, clen, _, _, attr, loff) = \
            struct.unpack("<IHHHHHHIIIHHHHHII", cd[p:p + 46])
        name = cd[p + 46:p + 46 + nlen].decode("utf-8", "replace")
        extra = cd[p + 46 + nlen:p + 46 + nlen + elen]
        if 0xFFFFFFFF in (csize, usize, loff):  # zip64 extra
            q = 0
            while q + 4 <= len(extra):
                hid, hsz = struct.unpack("<HH", extra[q:q + 4])
                if hid == 1:
                    f = extra[q + 4:q + 4 + hsz]
                    k = 0
                    if usize == 0xFFFFFFFF:
                        usize = struct.unpack("<Q", f[k:k + 8])[0]; k += 8
                    if csize == 0xFFFFFFFF:
                        csize = struct.unpack("<Q", f[k:k + 8])[0]; k += 8
                    if loff == 0xFFFFFFFF:
                        loff = struct.unpack("<Q", f[k:k + 8])[0]; k += 8
                q += 4 + hsz
        entries.append((name, method, csize, usize, crc, attr >> 16, loff))
        p += 46 + nlen + elen + clen
    return entries


def make_selector(ver, api):
    base = "android-ndk-r%s/toolchains/llvm/prebuilt/linux-x86_64/" % ver
    inc = base + "sysroot/usr/include/"
    lib = base + "sysroot/usr/lib/%s/" % TRIPLE
    keep_top = {"libc++_static.a", "libc++abi.a"}
    skip_api = {"libc++.a", "libc++.so"}  # скрипты на libc++_shared - нам нужна только статика
    res = base + "lib/clang/"

    def sel(name):
        if name.endswith("/"):
            return None
        if name.startswith(inc):
            return "usr/include/" + name[len(inc):]
        if name.startswith(lib):
            rel = name[len(lib):]
            if "/" not in rel:
                return "usr/lib/%s/%s" % (TRIPLE, rel) if rel in keep_top else None
            d, f = rel.split("/", 1)
            if d == str(api) and "/" not in f and f not in skip_api:
                return "usr/lib/%s/%s/%s" % (TRIPLE, d, f)
            return None
        if name.startswith(res) and "/lib/linux/aarch64/" in name:
            f = name.rsplit("/", 1)[1]
            if f == "libunwind.a":
                return "usr/lib/%s/%s" % (TRIPLE, f)
        return None

    return sel


def extract(url, entries, wanted, out):
    wanted.sort(key=lambda t: t[1][6])  # по смещению в архиве
    groups, cur = [], []
    for item in wanted:
        e = item[1]
        if cur:
            prev = cur[-1][1]
            prev_end = prev[6] + 30 + 1024 + prev[2]
            if e[6] - prev_end > GAP or (e[6] + e[2] + 4096) - cur[0][1][6] > CHUNK_MAX:
                groups.append(cur)
                cur = []
        cur.append(item)
    if cur:
        groups.append(cur)
    done = 0
    for g in groups:
        start = g[0][1][6]
        end = g[-1][1][6] + 30 + 65536 + g[-1][1][2]  # заголовок + запас на name/extra + данные
        blob = http_range(url, start, end)
        for dest, (name, method, csize, usize, crc, mode, loff) in g:
            o = loff - start
            if blob[o:o + 4] != b"PK\x03\x04":
                raise RuntimeError("битый локальный заголовок: " + name)
            nlen, elen = struct.unpack("<HH", blob[o + 26:o + 30])
            ds = o + 30 + nlen + elen
            data = blob[ds:ds + csize]
            if len(data) != csize:
                raise RuntimeError("обрезанные данные: " + name)
            if method == 8:
                data = zlib.decompress(data, -15)
            elif method != 0:
                raise RuntimeError("метод сжатия %d: %s" % (method, name))
            if (zlib.crc32(data) & 0xFFFFFFFF) != crc:
                raise RuntimeError("CRC не сошёлся: " + name)
            path = os.path.join(out, dest)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            if (mode & 0o170000) == 0o120000:  # символическая ссылка
                if os.path.lexists(path):
                    os.remove(path)
                os.symlink(data.decode(), path)
            else:
                with open(path, "wb") as f:
                    f.write(data)
                os.chmod(path, 0o755 if (mode & 0o111) else 0o644)
            done += 1
        log("  ... %d/%d файлов" % (done, len(wanted)))
    return done


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    ver, out = sys.argv[1], os.path.abspath(sys.argv[2])
    api = int(sys.argv[3]) if len(sys.argv) > 3 else 29
    url = NDK_URL.format(ver=ver)
    log("NDK r%s: читаю оглавление архива" % ver)
    entries = read_central_directory(url)
    sel = make_selector(ver, api)
    wanted = []
    for e in entries:
        d = sel(e[0])
        if d:
            wanted.append((d, e))
    if not wanted:
        raise RuntimeError("в архиве нет ожидаемых путей (изменилась раскладка NDK r%s?)" % ver)
    log("нужно файлов: %d, размер в архиве ~%d МБ" % (len(wanted), sum(e[2] for _, e in wanted) >> 20))
    tmp = out + ".part"
    if os.path.isdir(tmp):
        import shutil
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    extract(url, entries, wanted, tmp)
    # проверка ключевого содержимого
    must = ["usr/include/vulkan/vulkan.h", "usr/include/c++/v1/string", "usr/include/android/surface_control.h",
            "usr/lib/%s/libc++_static.a" % TRIPLE, "usr/lib/%s/libc++abi.a" % TRIPLE,
            "usr/lib/%s/libunwind.a" % TRIPLE, "usr/lib/%s/%d/libandroid.so" % (TRIPLE, api),
            "usr/lib/%s/%d/liblog.so" % (TRIPLE, api), "usr/lib/%s/%d/libnativewindow.so" % (TRIPLE, api),
            "usr/lib/%s/%d/libsync.so" % (TRIPLE, api), "usr/lib/%s/%d/crtbegin_so.o" % (TRIPLE, api)]
    miss = [m for m in must if not os.path.exists(os.path.join(tmp, m))]
    if miss:
        raise RuntimeError("после распаковки не хватает: " + ", ".join(miss))
    # Не все сборки clang ищут подкаталог с номером API (usr/lib/<triple>/<api>/): дублируем
    # заглушки и crt*.o на уровень выше - тогда они находятся в любом случае.
    import shutil
    apidir = os.path.join(tmp, "usr/lib/%s/%d" % (TRIPLE, api))
    for fn in os.listdir(apidir):
        dst = os.path.join(tmp, "usr/lib/%s/%s" % (TRIPLE, fn))
        if not os.path.exists(dst):
            shutil.copy2(os.path.join(apidir, fn), dst)
    with open(os.path.join(tmp, "NDK_VERSION"), "w") as f:
        f.write("r%s api%d\n" % (ver, api))
    if os.path.isdir(out):
        import shutil
        shutil.rmtree(out)
    os.rename(tmp, out)
    log("sysroot готов: " + out)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        log("ОШИБКА: %s" % exc)
        sys.exit(1)
