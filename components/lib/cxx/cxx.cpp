// C++ runtime for esp-ndf (no libstdc++): heap-backed new/delete, pure-virtual trap, the bits the ABI needs.
#include <stddef.h>
#include <new>
extern "C" void *malloc(size_t); extern "C" void free(void *); extern "C" void abort(void);   // components/lib/heap.c, components/lib/stdlib.c

void *operator new(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }   // no exceptions: fail hard
void *operator new[](size_t n) { return operator new(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return malloc(n ? n : 1); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return malloc(n ? n : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" {
void *__dso_handle = 0;
void __cxa_pure_virtual(void) { abort(); }
int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }   // statics live for ever (no exit)
}
