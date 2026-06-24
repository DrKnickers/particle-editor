// Regression test for the XML entity-expansion DoS guard (src/xml.cpp, audit F-XML).
//
// The bundled Expat predates the 2.4.0 billion-laughs amplification cap and has
// no entity-expansion limit. Legit game/mod XML declares NO custom entities, so
// the fix registers an entity-decl handler that XML_StopParser's on the first
// <!ENTITY> declaration -- the aborted parse surfaces as the normal
// XML_Parse==0 ParseException. This test feeds a classic billion-laughs payload
// (nested internal entities) through XMLTree::parse via a MemoryFile and asserts
// it THROWS promptly (does not hang / expand), while a normal small document
// parses WITHOUT throwing. See tests/build_test_xml_billion_laughs.bat.

#include "xml.h"
#include "files.h"
#include "exceptions.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

// Seed a MemoryFile with `text` (UTF-8 bytes) rewound to 0 for parse()'s reads.
static MemoryFile* makeFile(const std::string& text)
{
    MemoryFile* f = new MemoryFile();   // rc=1
    if (!text.empty()) f->write(text.data(), (unsigned long)text.size());
    f->seek(0);
    return f;
}

int main()
{
    std::printf("test_xml_billion_laughs\n");

    // --- A: billion-laughs payload must be REJECTED (parse throws), not expand/hang.
    // Classic nested internal entities: each level references the one below 10x.
    // Even a few levels is exponential; the entity-decl guard aborts on the FIRST
    // <!ENTITY> so this never expands at all.
    {
        const std::string bomb =
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE lolz [\n"
            "  <!ENTITY lol \"lol\">\n"
            "  <!ENTITY lol1 \"&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;\">\n"
            "  <!ENTITY lol2 \"&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;\">\n"
            "  <!ENTITY lol3 \"&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;\">\n"
            "  <!ENTITY lol4 \"&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;\">\n"
            "  <!ENTITY lol5 \"&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;\">\n"
            "  <!ENTITY lol6 \"&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;\">\n"
            "  <!ENTITY lol7 \"&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;\">\n"
            "  <!ENTITY lol8 \"&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;\">\n"
            "  <!ENTITY lol9 \"&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;\">\n"
            "]>\n"
            "<lolz>&lol9;</lolz>\n";

        MemoryFile* f = makeFile(bomb);   // rc=1
        XMLTree tree;
        bool threw = false;
        try
        {
            tree.parse(f);                // must abort on the first <!ENTITY>
        }
        catch (wexception&)               // ParseException derives from wexception
        {
            threw = true;
        }
        catch (...)
        {
            threw = true;
        }
        f->Release();                     // rc=0
        CHECK(threw, "billion-laughs payload is rejected (parse throws, no expansion/hang)");
    }

    // --- B: a normal small document parses WITHOUT throwing.
    {
        const std::string ok = "<root><a/></root>";
        MemoryFile* f = makeFile(ok);     // rc=1
        XMLTree tree;
        bool threw = false;
        try
        {
            tree.parse(f);
        }
        catch (...)
        {
            threw = true;
        }
        CHECK(!threw, "normal small document parses without throwing");
        // And the tree is actually populated (root element is <root>).
        bool rootOk = false;
        if (!threw && tree.getRoot() != NULL)
            rootOk = (tree.getRoot()->getName() == L"root");
        CHECK(rootOk, "normal document yields a <root> element");
        f->Release();                     // rc=0
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
