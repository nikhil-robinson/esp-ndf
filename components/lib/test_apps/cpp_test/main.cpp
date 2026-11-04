// C++ on esp-ndf: global constructors (with priorities), virtual calls, new/delete, templates, std headers
// that need no runtime (array, algorithm), function-local statics (no thread-safe guards: -fno-threadsafe-statics).
#include <array>
#include <algorithm>
extern "C" {
#include "console.h"
#include "sys.h"
#include "cpu.h"
#include "heap.h"
}

static int order[4], n_order;
struct Rec { int id; explicit Rec(int i) : id(i) { order[n_order++] = i; } };
static Rec r_late __attribute__((init_priority(2000))) (2);
static Rec r_early __attribute__((init_priority(1000))) (1);
static Rec r_plain(3);

struct Shape { virtual ~Shape() {} virtual int area() const = 0; };
struct Rect : Shape { int w, h; Rect(int a, int b) : w(a), h(b) {} int area() const override { return w * h; } };
struct Sq : Rect { explicit Sq(int a) : Rect(a, a) {} };

template <typename T, size_t N> T sum(const std::array<T, N> &a) { T s{}; for (auto v : a) s += v; return s; }

static int counter() { static int c = 100; return ++c; }

extern "C" int main(void)
{
    int fails = 0;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
    CHECK(n_order == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3, "global constructors ran in priority order: %d %d %d", order[0], order[1], order[2]);
    size_t h0 = heap_free();
    Shape *s[3] = { new Rect(3, 4), new Sq(5), new Rect(2, 8) };
    int tot = 0;
    for (auto p : s) tot += p->area();
    uint32_t c0 = ccount(); int a = s[1]->area(); uint32_t vc = ccount() - c0;
    for (auto p : s) delete p;
    CHECK(tot == 12 + 25 + 16 && a == 25 && heap_free() == h0, "virtual area() sum %d, heap back to start after delete (call %lu cycles)", tot, (unsigned long)vc);
    std::array<int, 6> v{ 5, 3, 9, 1, 7, 2 };
    std::sort(v.begin(), v.end());
    CHECK(v[0] == 1 && v[5] == 9 && sum(v) == 27 && std::min(4, 9) == 4, "std::array + std::sort + template sum %d", sum(v));
    CHECK(counter() == 101 && counter() == 102, "function-local static");
    printf("=== cpp_test %s (%d failures) ===\n", fails ? "FAILED" : "ALL PASS", fails);
    for (;;) {}
}
