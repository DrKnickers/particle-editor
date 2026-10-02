// Tests for the duplicate-name rule (src/EmitterNaming.h).

#include "EmitterNaming.h"

#include <cstdio>

static int g_failed = 0;

static void CheckName(const std::vector<std::string>& existingNames,
                      const std::string& sourceName,
                      const std::string& expected, const char* msg)
{
    const std::string got = NextDuplicateName(existingNames, sourceName);
    if (got == expected)
    {
        std::printf("  ok: %s\n", msg);
    }
    else
    {
        ++g_failed;
        std::printf("  FAIL: %s (got %s, expected %s)\n",
                    msg, got.c_str(), expected.c_str());
    }
}

int main()
{
    std::printf("test_emitter_naming\n");

    CheckName({"Foo"}, "Foo", "Foo_1", "unsuffixed name counts as zero");
    CheckName({"Foo_3"}, "Foo_3", "Foo_4", "existing source suffix is stripped and counted");
    CheckName({}, "Foo_3", "Foo_1", "source suffix alone does not reserve a number");
    CheckName({"Foo", "Foo_1", "Foo_7"}, "Foo", "Foo_8", "gaps do not reuse a lower number");
    CheckName({}, "Foo_", "Foo__1", "trailing underscore without digits is retained");
    CheckName({"Foo_3_1"}, "Foo_3_1", "Foo_3_2", "only one trailing suffix is stripped");
    CheckName({"FooBar_2"}, "Foo", "Foo_1", "longer prefix-sharing name does not count");
    CheckName({}, "", "_1", "empty name gets the first suffix");
    CheckName({}, "Foo", "Foo_1", "empty list starts at one");
    CheckName({"Foo_003", "Foo_07"}, "Foo_003", "Foo_8", "leading-zero suffixes count numerically");
    CheckName({"Foo_0"}, "Foo_0", "Foo_1", "zero suffix counts as zero");
    CheckName({"Foo_", "Foo_2x", "Foo_-9", "Foo_3_1"}, "Foo", "Foo_1",
              "only an all-digit suffix on the same base counts");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
