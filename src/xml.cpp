#include <iostream>
#include <fstream>
#include "xml.h"
#include "exceptions.h"
#include "utils.h"
#include "ResourceLimits.h"
using namespace std;

static const int BUFFER_SIZE = 32*1024;	// Read 32k at once

XMLNode::XMLNode(XMLNode* parent, const XML_Char* name, const XML_Char **atts)
{
	this->parent   = parent;
	this->name     = name;

	// Expat passes attributes as a NULL-terminated [name0, val0, name1,
	// val1, ...] array. Advance by pairs — without the `atts += 2` this
	// loop spun forever (100% CPU) on any element carrying >=1 attribute
	// (audit G10). The `atts[1]` guard also tolerates a malformed
	// odd-length array rather than reading past the terminator.
	while (atts && atts[0] && atts[1])
	{
		attributes.insert(make_pair(atts[0], atts[1]));
		atts += 2;
	}
}

XMLNode::XMLNode(XMLNode* parent, const XML_Char* s, int len) : data(s, len)
{
	this->parent = parent;
}

// Iterative teardown: a recursive delete (each child's destructor deleting its
// own children) consumes one stack frame per nesting level, so a deeply nested
// tree — even one under kMaxXmlDepth on a small worker-pool stack — risks
// overflow. Flatten the subtree into an explicit worklist instead; each child's
// own destructor then runs with an empty children vector and never recurses.
XMLNode::~XMLNode()
{
	vector<XMLNode*> stack(children.begin(), children.end());
	children.clear();
	while (!stack.empty())
	{
		XMLNode* n = stack.back();
		stack.pop_back();
		stack.insert(stack.end(), n->children.begin(), n->children.end());
		n->children.clear();
		delete n;
	}
}

static wstring trim(const wstring& source, const wchar_t* delims = L" \t\r\n")
{
	wstring result(source);
	wstring::size_type index = result.find_last_not_of(delims);
	if (index != wstring::npos)
		result.erase(index + 1);

	index = result.find_first_not_of(delims);
	if(index != wstring::npos) 
		result.erase(0, index);
	else
		result.erase();
	return result;
}

static void checkEmpty(XMLNode* node)
{
	if (node->children.size() > 0)
	{
		XMLNode* child = node->children.back();
		child->data = trim(child->data);
		if (child->data.empty() && child->name.empty())
		{
			delete child;
			node->children.erase( node->children.end() - 1 );
		}
	}
}

// Audit F-XML (untrusted mod XML): the bundled Expat is 2.1.0, with no
// entity-expansion limit and no nesting limit. Both handlers below stop the
// parser rather than throw — C++ exceptions must not unwind through Expat's C
// frames. thread_local: XMLTree::parse runs concurrently on the
// GameObjectCatalog worker pool, so this MUST be per-thread — a shared static
// would let one thread's handler stop another thread's parser (DoS bypass) or
// read a freed parser. Each parse() sets + clears both within one call stack.
static thread_local XML_Parser    g_xmlParser = NULL;
static thread_local unsigned long g_xmlDepth  = 0;

static void onStartElement(void* userData, const XML_Char *name, const XML_Char **atts)
{
	// Depth cap for untrusted mod XML: a crafted file nesting tens of thousands
	// of elements deep would otherwise build an arbitrarily tall node chain (and
	// the game's own XML never nests remotely this deep). Stop the parser BEFORE
	// allocating the node; the aborted parse surfaces as the normal
	// XML_Parse==0 ParseException in XMLTree::parse.
	if (++g_xmlDepth > kMaxXmlDepth)
	{
		if (g_xmlParser != NULL) XML_StopParser(g_xmlParser, XML_FALSE);
		return;
	}
	XMLTree* tree = (XMLTree*)userData;
	XMLNode* node = new XMLNode(tree->current, name, atts);
	if (tree->current == NULL)
	{
		if (tree->root == NULL)
		{
			delete tree->root;
		}
		tree->root = node;
	}
	else
	{
		checkEmpty(tree->current);
		tree->current->children.push_back( node );
	}
	tree->current = node;
}

static void onEndElement(void* userData, const XML_Char *name)
{
	if (g_xmlDepth > 0) --g_xmlDepth;
	XMLTree* tree = (XMLTree*)userData;
	if (tree->current != NULL)
	{
		// Post-process this node; if it contains a single anonymous child, put it into
		// this node's data field
		if ((tree->current->children.size() == 1) && (tree->current->children.front()->name.empty()))
		{
			tree->current->data = tree->current->children.front()->data;
			delete tree->current->children.front();
			tree->current->children.clear();
		}
		tree->current->data = trim(tree->current->data);

		checkEmpty(tree->current);
		tree->current = tree->current->parent;
	}
}

static void onCharacterData(void *userData, const XML_Char *s, int len)
{
	XMLTree* tree = (XMLTree*)userData;
	if (tree->current != NULL)
	{
		if ((tree->current->children.size() > 0) && (tree->current->children.back()->name.empty()))
		{
			tree->current->children.back()->data += wstring(s, len);
		}
		else
		{
			tree->current->children.push_back( new XMLNode(tree->current, s, len) );
		}
	}
}

// Expat recognizes "US-ASCII" but NOT the bare "ASCII" that many mod XML files
// declare (one mod's core folder ships 11 of 33 files as <?xml ... encoding='ASCII'?>). Left
// unhandled, XML_Parse aborts with "unknown encoding" and XMLTree::parse throws --
// and the catalog's parseObjectFile / parseHardpointFile swallow that throw, silently
// dropping EVERY game object in the file (and any Variant_Of parent defined there).
// The game tolerates "ASCII"; mirror it. Map the name (case-insensitive ASCII /
// US-ASCII) to an identity byte->codepoint table -- ASCII is a strict subset, and a
// stray high byte degrades to Latin-1 rather than aborting the whole file. Decline
// other unknown names so genuine UTF-16 / Latin-1 declarations keep expat's handling.
static int onUnknownEncoding(void* /*data*/, const XML_Char* name, XML_Encoding* info)
{
	auto ieq = [](const XML_Char* a, const wchar_t* b) -> bool {
		for (; *a && *b; ++a, ++b)
		{
			const wchar_t ca = (*a >= L'A' && *a <= L'Z') ? (wchar_t)(*a + 32) : (wchar_t)*a;
			const wchar_t cb = (*b >= L'A' && *b <= L'Z') ? (wchar_t)(*b + 32) : *b;
			if (ca != cb) return false;
		}
		return *a == 0 && *b == 0;
	};
	if (!ieq(name, L"ascii") && !ieq(name, L"us-ascii"))
		return XML_STATUS_ERROR;   // not ours -> let expat reject genuinely unknown encodings
	for (int i = 0; i < 256; ++i) info->map[i] = i;   // identity byte->codepoint (ASCII subset; Latin-1 fallback)
	info->data    = NULL;
	info->convert = NULL;
	info->release = NULL;
	return XML_STATUS_OK;
}

// Legit game/mod XML declares NO custom entities (the bundled Expat 2.1.0
// predates the 2.4.0 billion-laughs amplification cap), so abort parsing on the
// first <!ENTITY> declaration — closing the entity-expansion DoS. The aborted
// parse surfaces as the normal XML_Parse==0 ParseException below.
static void onEntityDecl(void* /*userData*/, const XML_Char* /*entityName*/,
    int /*isParameterEntity*/, const XML_Char* /*value*/, int /*valueLength*/,
    const XML_Char* /*base*/, const XML_Char* /*systemId*/,
    const XML_Char* /*publicId*/, const XML_Char* /*notationName*/)
{
    if (g_xmlParser != NULL) XML_StopParser(g_xmlParser, XML_FALSE);
}

void XMLTree::parse(IFile* file)
{
	// Reset tree
	delete root;
	root    = NULL;
	current = NULL;

	XML_Parser parser = XML_ParserCreate(NULL);
	if (parser == NULL)
	{
		throw wruntime_error(LoadString(IDS_ERROR_XML_PARSER_CREATE));
	}

	XML_SetUserData(parser, this);
	XML_SetElementHandler(parser, onStartElement, onEndElement);
	XML_SetCharacterDataHandler(parser, onCharacterData);
	XML_SetUnknownEncodingHandler(parser, onUnknownEncoding, NULL);   // tolerate encoding='ASCII'
	g_xmlParser = parser;   // F-XML: for onEntityDecl's / onStartElement's XML_StopParser
	g_xmlDepth  = 0;        // fresh depth per parse (thread_local survives across calls)
	XML_SetEntityDeclHandler(parser, onEntityDecl);        // F-XML: reject custom entity declarations

	try
	{
		// F-XML: cap total input so a crafted untrusted mod XML can't drive an
		// unbounded read/parse. Real game XML is well under this.
		unsigned long total = 0;
		while (!file->eof())
		{
			char buffer[ BUFFER_SIZE ];
			unsigned long n = file->read(buffer, BUFFER_SIZE);
			total += n;
			if (total > kMaxXmlFileBytes)
			{
				throw ParseException( LoadString(IDS_ERROR_XML, L"input too large", 0) );
			}
			if (XML_Parse(parser, buffer, n, file->eof()) == 0)
			{
				const wstring error = XML_ErrorString(XML_GetErrorCode(parser));
                throw ParseException( LoadString(IDS_ERROR_XML, error.c_str(), XML_GetCurrentLineNumber(parser)) );
			}
		}
	}
	catch (...)
	{
		g_xmlParser = NULL;
		XML_ParserFree(parser);
		throw;
	}
	g_xmlParser = NULL;
	XML_ParserFree(parser);
}

XMLTree::XMLTree()
{
	root    = NULL;
	current = NULL;
}

XMLTree::~XMLTree()
{
	delete root;
}
