// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/app.hpp>
#include <string>
namespace pxa::ext::wifi_location {
constexpr std::uint16_t service_id = 33027;
constexpr std::string_view permission = "org.doit.wifi-location", scope = "wifi.scan";
constexpr std::array<std::byte,20> hello{
    std::byte{0x2b},std::byte{0xdd},std::byte{0x0e},std::byte{0x91},std::byte{0xe1},std::byte{0xc4},std::byte{0x48},std::byte{0xda},
    std::byte{0x99},std::byte{0xe2},std::byte{0x6a},std::byte{0x70},std::byte{0xf3},std::byte{0x11},std::byte{0x39},std::byte{0x85},
    std::byte{0},std::byte{0},std::byte{1},std::byte{0}};
inline Task<void> open(Context& ctx) {
    auto e = co_await ctx.request(service_id, 1, hello);
    if (!e) co_return std::unexpected(e.error());
    auto b = wire::result_body(e->payload);
    if (!b || b->size() != hello.size() || !std::equal(b->begin(),b->end(),hello.begin()))
        co_return std::unexpected(Error::protocol_error);
    co_return Result<void>{};
}
struct Snapshot { std::uint64_t timestamp_us; unsigned count; std::string json; };
inline std::string quote(std::span<const std::byte> bytes) {
    constexpr char hex[]="0123456789abcdef";std::string result="\"";
    for(auto byte:bytes){const auto c=unsigned(byte);
        if(c=='"'||c=='\\'){result+='\\';result+=char(c);}
        else if(c<32){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}
        else result+=char(c);
    }
    return result+'"';
}
inline std::string access_point(const std::byte* mac,std::int8_t signal,std::span<const std::byte> ssid) {
    constexpr char hex[]="0123456789abcdef";std::string out="{\"macAddress\":\"";
    for(unsigned i=0;i<6;++i){if(i)out+=':';const auto b=unsigned(mac[i]);out+=hex[b>>4];out+=hex[b&15];}
    out+="\",\"signalStrength\":";int n=signal;if(n<0){out+='-';n=-n;}
    if(n>=100)out+=char('0'+n/100);
    if(n>=10)out+=char('0'+n/10%10);
    out+=char('0'+n%10);out+=",\"ssid\":";out+=quote(ssid);out+='}';return out;
}
inline Task<Snapshot> snapshot(Context& ctx, const Permission& grant) {
    std::array<std::byte,8> payload; wire::put64(payload.data(),grant.handle());
    auto e = co_await ctx.request(service_id, 2, payload);
    if (!e) co_return std::unexpected(e.error());
    auto b = wire::result_body(e->payload);
    if (!b) co_return std::unexpected(b.error());
    if (b->size() < 10) co_return std::unexpected(Error::protocol_error);
    const auto count = wire::get16(b->data()+8);
    if (count > 10) co_return std::unexpected(Error::protocol_error);
    Snapshot result{wire::get64(b->data()),0,"{\"considerIp\":false,\"fallbacks\":{\"ipf\":false,\"lacf\":false},\"wifiAccessPoints\":["};
    std::size_t at=10;
    for (unsigned i=0;i<count;++i) {
        if(b->size()-at<8)co_return std::unexpected(Error::protocol_error);
        const auto* p=b->data()+at;const auto size=unsigned(p[7]);
        if(!size||size>32||b->size()-at-8<size)co_return std::unexpected(Error::protocol_error);
        auto ssid=b->subspan(at+8,size);at+=8+size;
        if(!wire::valid_utf8(ssid))continue;
        if(result.count)result.json+=',';
        result.json+=access_point(p,std::int8_t(p[6]),ssid);++result.count;
    }
    if(at!=b->size())co_return std::unexpected(Error::protocol_error);
    if(result.count<2)co_return std::unexpected(Error::unavailable);
    result.json+="]}"; co_return result;
}
}
