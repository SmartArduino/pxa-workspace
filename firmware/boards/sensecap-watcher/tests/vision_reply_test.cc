// SPDX-License-Identifier: Apache-2.0
#include "watcher_vision_reply.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdio>
static pxa_status_t Decode(std::string text, uint16_t format, size_t capacity = 262144) {
    std::vector<uint8_t> output(capacity);
    size_t size = 0;
    const auto status = WatcherDecodeVisionReply(text.data(), text.size(), format, output.data(), capacity, &size);
    if (status == PXA_STATUS_OK) assert(size > 0 && size <= capacity);
    return status;
}
int main(int argc, char** argv) {
    const std::string valid = R"({"type":1,"name":"SAMPLE","code":0,"data":{"image":"/9j/2Q==","resolution":[240,240]}})";
    assert(Decode(valid, PXA_VISION_JPEG) == PXA_STATUS_OK);
    assert(Decode(R"({"type":1,"name":"SAMPLE","code":0,"data":{"image":"/9j/2QAAAAA="}})", 1) == PXA_STATUS_OK);
    assert(Decode(R"({"type":1,"name":"SAMPLE","code":0,"data":{"image":"/9j/2QAB"}})", 1) == PXA_STATUS_PROTOCOL_ERROR);
    assert(Decode(valid, PXA_VISION_JSON) == PXA_STATUS_PROTOCOL_ERROR);
    assert(Decode(valid, PXA_VISION_JPEG, 1) == PXA_STATUS_PROTOCOL_ERROR);
    assert(Decode(R"({"type":0,"name":"SAMPLE","code":0,"data":{}})", 1) == PXA_STATUS_WOULD_BLOCK);
    assert(Decode(R"({"type":0,"name":"SAMPLE","code":1,"data":{}})", 1) == PXA_STATUS_UNAVAILABLE);
    assert(Decode(R"({"type":1,"name":"SAMPLE","code":0,"data":{}})", 1) == PXA_STATUS_PROTOCOL_ERROR);
    assert(Decode(R"({"type":1,"name":"INVOKE","code":0,"data":{"boxes":[[12,23,34,45,99,1]],"perf":[10,20,3]}})", 2) == PXA_STATUS_OK);
    assert(Decode(R"({"type":1,"name":"INVOKE","code":0,"data":{"boxes":[],"image":""}})", 2) == PXA_STATUS_OK);
    assert(Decode(R"({"type":1,"name":"INVOKE","code":0,"data":{"image":"/9j/2Q=="}})", 2) == PXA_STATUS_PROTOCOL_ERROR);
    assert(Decode(R"({"type":1,"name":"SAMPLE","code":0,"data":{"image":"bad!"}})", 1) == PXA_STATUS_PROTOCOL_ERROR);
    assert(Decode(R"({"type":1,"name":"SAMPLE","code":0,"data":{"image":"/9j/2Q==","image":"/9j/2Q=="}})", 1) == PXA_STATUS_PROTOCOL_ERROR);
    assert(Decode(R"({"type":1,"name":"SAMPLE","code":0,"data":{"image":"/9j\/2Q=="}})", 1) == PXA_STATUS_PROTOCOL_ERROR);
    assert(Decode(R"({"type":1,"name":"OTHER","code":0,"data":{}})", 1) == PXA_STATUS_WOULD_BLOCK);
    assert(Decode(std::string(13, '[') + std::string(13, ']'), 1) == PXA_STATUS_PROTOCOL_ERROR);
    assert(Decode(R"({"type":1,"name":"SAMPLE","code":0,"data":{"image":"/9j/2Q=="}}trailing)", 1) == PXA_STATUS_PROTOCOL_ERROR);
    if (argc > 1) {
        std::ifstream f(argv[1]); assert(f.good());
        std::string line; unsigned images = 0;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            while (!line.empty() && (line.front() == '\r' || line.front() == ' ')) line.erase(0, 1);
            if (line.empty() || line[0] != '{') continue;
            const auto status = Decode(line, PXA_VISION_JPEG);
            assert(status == PXA_STATUS_OK || status == PXA_STATUS_WOULD_BLOCK);
            if (status == PXA_STATUS_OK) ++images;
        }
        assert(images == 1);
    }
    if (argc > 2) {
        std::ifstream f(argv[2]); assert(f.good());
        std::string line; unsigned results = 0;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            while (!line.empty() && (line.front() == '\r' || line.front() == ' ')) line.erase(0, 1);
            if (line.empty() || line[0] != '{') continue;
            const auto status = Decode(line, PXA_VISION_JSON);
            assert(status == PXA_STATUS_OK || status == PXA_STATUS_WOULD_BLOCK);
            if (status == PXA_STATUS_OK) ++results;
        }
        assert(results == 1);
    }
    puts("Himax reply parser: actual device JPEG, bounded decoding, malformed metadata/base64/depth/duplicates and inference JSON passed");
}
