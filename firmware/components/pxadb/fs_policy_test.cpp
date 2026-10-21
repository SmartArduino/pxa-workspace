#include "pxadb_fs_policy.h"
#include <cassert>
#include <string>

static void test_fs_paths() {
    for (const auto path : {".", "pxa-state/inbox/test.pxa", "sdcard/PXA/media/nes/game.nes",
                            "sdcard/PXA/media/nes/金庸群侠传.书剑江山.nes",
                            "pxa-state/inbox/游戏/金庸群侠传.书剑江山.nes",
                            "sdcard/游戏/中文 (修订版) [汉化].nes",
                            "sdcard/café/日本語/🎮.nes"}) {
        assert(pxadb::valid_fs_path(path));
    }
    for (const auto path : {"", "/", "/sdcard/game.nes", "sdcard/", "sdcard//game.nes",
                            "..", "../game.nes", "sdcard/./game.nes", "sdcard/../game.nes",
                            "sdcard/游戏/..", "sdcard\\game.nes", "sdcard/C:game.nes",
                            "sdcard/line\nfeed.nes", "sdcard/tab\tname.nes", "sdcard/\x7f.nes",
                            "sdcard/\x80.nes", "sdcard/\xc0\xaf.nes", "sdcard/\xe0\x80\xaf.nes",
                            "sdcard/\xed\xa0\x80.nes", "sdcard/\xf0\x80\x80\xaf.nes",
                            "sdcard/\xf4\x90\x80\x80.nes", "sdcard/\xf8\x80\x80\x80\x80.nes",
                            "sdcard/\xe9\x87", "sdcard/\xe9/rom.nes"}) {
        assert(!pxadb::valid_fs_path(path));
    }
    assert(!pxadb::valid_fs_path(std::string_view("sdcard/rom\0.nes", 15)));
    assert(pxadb::valid_fs_path(std::string(pxadb::kFsPathCapacity - 1, 'a')));
    assert(!pxadb::valid_fs_path(std::string(pxadb::kFsPathCapacity, 'a')));
    std::string chinese_path = "sdcard/";
    while (chinese_path.size() + 3 < pxadb::kFsPathCapacity) chinese_path += "中";
    assert(pxadb::valid_fs_path(chinese_path));
    assert(!pxadb::valid_fs_path(chinese_path + "中"));
}

int main() {
    test_fs_paths();
    for (const auto path : {"system/fonts", "system/pxa"}) {
        assert(!pxadb::may_create_directory(path, false, false));
        assert(pxadb::may_create_directory(path, false, true));
        assert(pxadb::protected_system_directory(path));
    }
    for (const auto path : {"system", "systematic/pxa", "pxa-state/packages", "../system/pxa"})
        assert(!pxadb::may_create_directory(path, false, true));
    assert(pxadb::may_create_directory("pxa-state/inbox/test", true, false));
}
