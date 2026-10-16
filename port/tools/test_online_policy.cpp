#include "../src/runtime/kernel/online_policy.h"
#include <cassert>
#include <initializer_list>

int main()
{
    for (const char* invalid : {"", "0", "480", "0480", "-1", "+123", " 123", "123x",
                                "4294967296", "999999999999999999999999"})
        assert(net::ParseSteamAppId(invalid) == 0);
    assert(net::ParseSteamAppId(nullptr) == 0);
    assert(net::ParseSteamAppId("123456") == 123456);
    assert(net::ParseSteamAppId("4294967295") == UINT32_MAX);
}
