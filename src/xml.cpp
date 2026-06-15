#include <iostream>
#include <fstream>
#include "xml.h"
#include "exceptions.h"
#include "utils.h"
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

XMLNode::~XMLNode()
{
	for (vector<XMLNode*>::iterator i = children.begin(); i != children.end(); i++)
	{
		delete *i;
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

static void onStartElement(void* userData, const XML_Char *name, const XML_Char **atts)
{
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
// declare (Mod Core ships 11 of 33 files as <?xml ... encoding='ASCII'?>). Left
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

	try
	{
		while (!file->eof())
		{
			char buffer[ BUFFER_SIZE ];
			unsigned long n = file->read(buffer, BUFFER_SIZE);
			if (XML_Parse(parser, buffer, n, file->eof()) == 0)
			{
				const wstring error = XML_ErrorString(XML_GetErrorCode(parser));
                throw ParseException( LoadString(IDS_ERROR_XML, error.c_str(), XML_GetCurrentLineNumber(parser)) );
			}
		}
	}
	catch (...)
	{
		XML_ParserFree(parser);
		throw;
	}
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
