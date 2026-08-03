// Regression test for the XML DoS guards (src/xml.cpp, audit F-XML + the
// 2026-07 pre-release audit).
//
// Guard 1 (entity expansion): the bundled Expat predates the 2.4.0
// billion-laughs amplification cap and has no entity-expansion limit. Legit
// game/mod XML declares NO custom entities, so the fix registers an entity-decl
// handler that XML_StopParser's on the first <!ENTITY> declaration -- the
// aborted parse surfaces as the normal XML_Parse==0 ParseException.
//
// Guard 2 (nesting depth): a crafted file nesting elements tens of thousands
// deep would otherwise build an arbitrarily tall XMLNode chain whose teardown
// once recursed one stack frame per level (stack overflow = hard crash during
// the startup catalog prefetch). onStartElement stops the parser past
// kMaxXmlDepth, and ~XMLNode tears down iteratively as a belt.
//
// Guard 3 (document attributes): XMLNode copies every Expat [name,value] pair
// into a std::map. A shallow document can therefore amplify many short
// attributes into map-node/string allocations while paying for only a handful
// of elements. The approved limit accepts exactly 131,072 pairs and rejects
// pair 131,073 across the whole document.
//
// This test feeds both payloads through XMLTree::parse via a MemoryFile and
// asserts they THROW promptly, while normal documents (including one nested
// deeper than any real game file but under the cap) parse WITHOUT throwing.
// See tests/build_test_xml_billion_laughs.bat.

#include "xml.h"
#include "files.h"
#include "exceptions.h"
#include "ResourceLimits.h"   // existing depth/breadth production constants

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

// Build a shallow document whose attributes are split across two siblings.
// This is deliberately NOT one giant element: a mistaken per-element cap would
// accept the over-budget document, while the required document-wide counter
// rejects it. Attribute names are unique within each element as XML requires.
static std::string makeAttributeDocument(unsigned long attributeCount)
{
    const unsigned long leftCount = attributeCount / 2;
    std::string xml;
    xml.reserve((size_t)attributeCount * 16u + 64u);
    xml += "<root><left";
    for (unsigned long i = 0; i < leftCount; ++i)
    {
        xml += " a";
        xml += std::to_string(i);
        xml += "=\"v\"";
    }
    xml += "/><right";
    for (unsigned long i = leftCount; i < attributeCount; ++i)
    {
        xml += " a";
        xml += std::to_string(i);
        xml += "=\"v\"";
    }
    xml += "/></root>";
    return xml;
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

    // --- C: nesting past kMaxXmlDepth (512) is REJECTED, promptly and without
    // a stack overflow. 50,000 levels: without the cap this built a 50k-node
    // chain whose recursive teardown overflowed the thread stack.
    {
        const int kDeep = 50000;
        std::string deep;
        deep.reserve((size_t)kDeep * 7 + 16);
        for (int i = 0; i < kDeep; ++i) deep += "<a>";
        for (int i = 0; i < kDeep; ++i) deep += "</a>";

        MemoryFile* f = makeFile(deep);   // rc=1
        XMLTree tree;
        bool threw = false;
        try
        {
            tree.parse(f);                // must stop at the depth cap
        }
        catch (...)
        {
            threw = true;
        }
        f->Release();                     // rc=0
        CHECK(threw, "50k-deep nesting is rejected (parse throws, no stack overflow)");
    }   // tree destructor runs here: also exercises iterative teardown of the partial tree

    // --- D: nesting deeper than any real game file but UNDER the cap parses
    // fine, and its teardown (400 levels) does not rely on recursion depth.
    {
        const int kOk = 400;
        std::string nested;
        nested.reserve((size_t)kOk * 7 + 16);
        for (int i = 0; i < kOk; ++i) nested += "<a>";
        for (int i = 0; i < kOk; ++i) nested += "</a>";

        MemoryFile* f = makeFile(nested); // rc=1
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
        CHECK(!threw, "400-deep nesting (under the cap) parses without throwing");
        CHECK(!threw && tree.getRoot() != NULL, "400-deep document yields a root");
        f->Release();                     // rc=0
    }

    // --- D: BREADTH. A shallow document with millions of siblings must be
    // rejected (2026-07 audit, B-1). The depth guard above does nothing here —
    // this document is two levels deep — and kMaxXmlFileBytes is no stand-in,
    // because every element becomes an XMLNode carrying a child vector and an
    // attribute map. The heap cost is a large multiple of the bytes on disk,
    // which is exactly the amplification the cap exists to stop.
    {
        std::string wide;
        wide.reserve((size_t)kMaxXmlNodes * 4 + 64);
        wide += "<root>";
        // <root> itself counts, so kMaxXmlNodes children guarantees we cross it.
        for (unsigned long i = 0; i < kMaxXmlNodes; ++i) wide += "<e/>";
        wide += "</root>";

        MemoryFile* f = makeFile(wide);
        XMLTree tree;
        bool threw = false;
        try { tree.parse(f); }
        catch (...) { threw = true; }
        f->Release();
        CHECK(threw, "millions of sibling elements are rejected (breadth cap)");
    }

    // --- E: a WIDE-but-legal document still parses. The cap has to sit above
    // anything real: the largest stock catalogs run to tens of thousands of
    // elements, so one an order of magnitude past that must be accepted.
    {
        std::string ok;
        ok += "<root>";
        for (int i = 0; i < 100000; ++i) ok += "<e/>";
        ok += "</root>";

        MemoryFile* f = makeFile(ok);
        XMLTree tree;
        bool threw = false;
        try { tree.parse(f); }
        catch (...) { threw = true; }
        CHECK(!threw, "100k sibling elements still parse (cap is above anything real)");
        CHECK(!threw && tree.getRoot() != NULL, "wide-but-legal document yields a root");
        f->Release();
    }

    // --- F: one below the approved literal attribute limit remains
    // accepted. Do not derive this fixture from kMaxXmlAttributes: a wrong
    // production constant must not move the test oracle with it.
    {
        const std::string xml = makeAttributeDocument(131071u);
        MemoryFile* f = makeFile(xml);
        XMLTree tree;
        bool threw = false;
        try { tree.parse(f); }
        catch (...) { threw = true; }
        CHECK(!threw, "131071 document-wide attributes parse (under contract)");
        CHECK(!threw && tree.getRoot() != NULL
              && tree.getRoot()->getNumChildren() == 2,
              "131071-attribute document yields both sibling elements");
        f->Release();
    }

    // --- G: the exact literal boundary is accepted. This kills an off-by-one
    // comparison and a cap-1 implementation.
    {
        const std::string xml = makeAttributeDocument(131072u);
        MemoryFile* f = makeFile(xml);
        XMLTree tree;
        bool threw = false;
        try { tree.parse(f); }
        catch (...) { threw = true; }
        CHECK(!threw, "131072 document-wide attributes parse (exact contract)");
        CHECK(!threw && tree.getRoot() != NULL
              && tree.getRoot()->getNumChildren() == 2,
              "131072-attribute document yields both sibling elements");
        f->Release();
    }

    // --- H: one over the literal boundary aborts the WHOLE document through
    // XML_StopParser and surfaces specifically as XMLTree's ParseException.
    // Catching any exception would allow an unrelated allocation/syntax failure
    // to masquerade as enforcement.
    {
        const std::string xml = makeAttributeDocument(131073u);
        MemoryFile* f = makeFile(xml);
        XMLTree tree;
        bool parseFailure = false;
        bool otherFailure = false;
        try { tree.parse(f); }
        catch (ParseException&) { parseFailure = true; }
        catch (...) { otherFailure = true; }
        CHECK(parseFailure,
              "131073 document-wide attributes raise ParseException (over contract)");
        CHECK(!otherFailure,
              "131073-attribute rejection is the parser-abort path, not another exception");
        f->Release();
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
