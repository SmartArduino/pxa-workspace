// Equivalent C/C++ encoding benchmark. Link runtime.cpp; C is used only here.
#include <pxa/game.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

static std::uint64_t imports, digest;
static unsigned allocations;
static std::array<unsigned char, 4096> captured;
static unsigned capture_size;
static bool capture;
void* operator new(std::size_t n) {++allocations; if (auto p=std::malloc(n)) return p; std::abort();}
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
extern "C" std::int32_t pxa_submit(const std::uint8_t*,std::uint32_t) {return 0;}
extern "C" std::int32_t pxa_io(std::uint64_t h,std::uint32_t op,std::uint8_t* p,std::uint32_t n) {
    assert(h==77 && op==0x101 && n==3112);
    ++imports;
    // Consume every byte, and make the imported function an optimizer barrier.
    std::uint64_t sum=0;
    for(unsigned i=0;i<n;++i) sum+=p[i];
    digest+=sum;
    if(capture) {std::memcpy(captured.data(),p,n);capture_size=n;}
    return n;
}
static pxa::game::DrawBuffer<4096> cpp_bytes;
static std::array<unsigned char,4096> c_bytes;
static pxa::Transport transport;
static pxa::game::Renderer renderer(transport,77,1);
extern "C" void pxa_sdk_benchmark_c_frame(std::uint8_t*,std::uint64_t);

[[gnu::noinline]] static void c_frame(std::uint64_t id) {
    pxa_sdk_benchmark_c_frame(c_bytes.data(),id);
}
[[gnu::noinline]] static void cpp_frame() {
    auto frame=renderer.frame(cpp_bytes);
    frame.clear({0x001f});
    for(int i=0;i<128;++i) {
        const auto x=static_cast<std::int16_t>((i%16)*256);
        const auto y=static_cast<std::int16_t>((i/16)*256);
        frame.quad({x,y,static_cast<std::int16_t>(x+128),y,
            static_cast<std::int16_t>(x+128),static_cast<std::int16_t>(y+128),x,static_cast<std::int16_t>(y+128)},
            {static_cast<std::uint16_t>(i*31)});
    }
    assert(frame.submit());
}
int main() {
    transport.phase(pxa::Phase::event);
    capture=true;cpp_frame();const auto expected=captured;
    c_frame(1);assert(capture_size==3112 && expected==captured);capture=false;
    for(int i=0;i<10000;++i) {c_frame(i+2);cpp_frame();}
    const auto before=allocations;
    constexpr int frames=200000;
    std::array<double,7> c_ns{},cpp_ns{};
    for(int run=0;run<7;++run) {
        for(int pass=0;pass<2;++pass) {
            const bool cpp=(pass+run)%2;
            auto begin=std::chrono::steady_clock::now();
            for(int i=0;i<frames;++i) {if(cpp) cpp_frame();else c_frame(i+2);}
            const auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/frames;
            (cpp?cpp_ns:c_ns)[run]=ns;
        }
        std::printf("{\"run\":%d,\"c_ns_per_frame\":%.3f,\"cpp_ns_per_frame\":%.3f,\"ratio\":%.4f}\n",run,c_ns[run],cpp_ns[run],cpp_ns[run]/c_ns[run]);
    }
    assert(allocations==before && imports==2+20000+7*2*frames);
    std::sort(c_ns.begin(),c_ns.end());std::sort(cpp_ns.begin(),cpp_ns.end());
    std::printf("{\"golden_equal\":true,\"payload_bytes\":3112,\"imports_per_frame\":1,\"steady_allocations\":%u,\"c_median_ns\":%.3f,\"cpp_median_ns\":%.3f,\"ratio\":%.4f,\"digest\":%llu}\n",allocations-before,c_ns[3],cpp_ns[3],cpp_ns[3]/c_ns[3],static_cast<unsigned long long>(digest));
}
