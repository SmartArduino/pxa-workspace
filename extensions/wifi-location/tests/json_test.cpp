// SPDX-License-Identifier: Apache-2.0
#include "../sdk/wifi_location.hpp"
#include <cassert>
#include <iostream>
int main(){
 using namespace pxa::ext::wifi_location;
 const std::array<std::byte,6> mac{std::byte{0},std::byte{1},std::byte{0x2a},std::byte{0x80},std::byte{0xef},std::byte{0xff}};
 const std::string name="AP\"\\\n中文";auto ssid=std::as_bytes(std::span{name.data(),name.size()});
 const auto value=access_point(mac.data(),-128,ssid);
 assert(value=="{\"macAddress\":\"00:01:2a:80:ef:ff\",\"signalStrength\":-128,\"ssid\":\"AP\\\"\\\\\\u000a中文\"}");
 for(int signal:{-99,-7,0,7,42,127}){
  const auto text=access_point(mac.data(),std::int8_t(signal),{});
  assert(text.find("\"signalStrength\":"+std::to_string(signal)+",")!=text.npos);
  assert(text.ends_with("\"ssid\":\"\"}"));
 }
 std::cout<<"PASS: MAC, RSSI boundaries, SSID quoting, UTF-8\n";
}
