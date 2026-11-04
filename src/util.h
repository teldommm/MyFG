#pragma once
#include <stddef.h>
#include <string>

std::string trim(const std::string& s);

// Чтение чужой памяти без риска SIGSEGV: write() в pipe возвращает EFAULT на неотображённых
// страницах. Глобальный pipe под мьютексом, len <= 4096.
bool safe_read(const void* addr, void* out, size_t len);

// Разрешение символа в уже загруженной библиотеке.
//  1) dlsym по RTLD_NOLOAD-хендлу (работает для публичных экспортов);
//  2) если не нашлось — разбор ELF-файла библиотеки с диска (.dynsym/.symtab) и расчёт
//     база + st_value. Это нужно для приватных символов libgui/libui: linker namespace
//     приложения не отдаёт их через dlsym.
// prefix=true: имя символа должно НАЧИНАТЬСЯ с name (для mangled-имён, где хвост сигнатуры
// меняется между версиями Android).
void* resolve_symbol(const char* lib_basename, const char* name, bool prefix);

// То же, но дополнительно возвращает размер функции (st_size из ELF; 0 — неизвестен).
// Размер нужен inline_hook(): патч Dobby занимает 12-16 байт и затирает соседей у коротких функций.
void* resolve_symbol_ex(const char* lib_basename, const char* name, bool prefix, size_t* size);

// Единственная точка установки inline-хуков (обёртка над DobbyHook).
//  * target_size > 0 и < 16  -> хук ОТКАЗАН (патч затёр бы соседние функции; типично для
//    экспортов libvulkan, которые являются 4-8-байтовыми tail-call заглушками);
//  * require_size=true и размер неизвестен -> хук отказан;
//  * пересечение с уже запатченным диапазоном -> отказ;
//  * перед патчем в лог пишутся адрес, размер и первые 16 байт функции (чтобы по логу было видно,
//    что именно перезаписывается).
// true — хук стоит, *orig — трамплин на оригинал.
bool inline_hook(const char* what, void* target, size_t target_size, bool require_size,
                 void* replace, void** orig);
