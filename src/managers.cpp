#include <algorithm>
#include <iostream>
#include "managers.h"
#include "exceptions.h"
#include "crc32.h"
#include "xml.h"
#include "utils.h"
using namespace std;

//
// FileManager class
//
IFile* FileManager::getFile(const string& path)
{
	// If a mod is selected, try its content roots first (mod root + bundled
	// sub-content folders like Mod's Core) so mod loose files shadow the
	// base game's. Root-first ordering preserves the prior single-modpath
	// behaviour; the extra roots only ADD reachability for sub-content.
	for (vector<wstring>::const_iterator root = modContentRoots.begin(); root != modContentRoots.end(); ++root)
	{
		try
		{
			wstring wpath = AnsiToWide(path);
			wstring filename = (path[1] != ':' && path[0] != '\\') ? *root + wpath : wpath;
			return new PhysicalFile(filename);
		}
		catch (IOException&)
		{
		}
	}

	// First see if we can open it physically
	for (vector<wstring>::const_iterator base = basepaths.begin(); base != basepaths.end(); base++)
	{
		try
		{
			wstring wpath = AnsiToWide(path);
			wstring filename = (path[1] != ':' && path[0] != '\\') ? *base + wpath : wpath;
			return new PhysicalFile(filename);
		}
		catch (IOException&)
		{
		}
	}

	// Search in the index
	try
	{
		for (vector<MegaFile*>::iterator i = megafiles.begin(); i != megafiles.end(); i++)
		{
			IFile* file = (*i)->getFile( path );
			if (file != NULL)
			{
				return file;
			}
		}
	}
	catch (IOException&)
	{
	}

	return NULL;
}

FileManager::FileManager(const vector<wstring>& basepaths)
{
	XMLTree xml;
	this->basepaths = basepaths;
	for (vector<wstring>::const_iterator path = basepaths.begin(); path != basepaths.end(); path++)
	{
		try
		{
			PhysicalFile* file = new PhysicalFile( *path + L"Data\\MegaFiles.xml" );
			xml.parse( file );
			file->Release();

			const XMLNode* root = xml.getRoot();
			if (root->getName() != L"Mega_Files")
			{
				throw BadFileException();
			}

			// Create a file index from all mega files
			for (unsigned int i = 0; i < root->getNumChildren(); i++)
			{
				const XMLNode* child = root->getChild(i);
				if (child->getName() != L"File")
				{
					// Tolerate non-<File> children (e.g. <Info Name=.../> in mod MegaFiles.xml):
					// skip them instead of throwing (the ctor's catch(...) would rethrow ->
					// std::terminate, crashing the editor on a standard-format mod).
					continue;
				}
		
				wstring filename = *path + child->getData();
				try
				{
					megafiles.push_back(new MegaFile(new PhysicalFile(filename)));
				}
				catch (IOException)
				{
				}
			}
		}
		catch (FileNotFoundException)
		{
			continue;
		}
		catch (...)
		{
			for (vector<MegaFile*>::iterator i = megafiles.begin(); i != megafiles.end(); i++)
			{
				delete *i;
			}
			throw;
		}
	}

	if (megafiles.empty())
	{
		throw FileNotFoundException(L"MegaFiles.xml");
	}
}

FileManager::~FileManager()
{
	for (vector<MegaFile*>::iterator i = megafiles.begin(); i != megafiles.end(); i++)
	{
		delete (*i);
	}
}

void FileManager::SetModPath(const wstring& path)
{
	modpath = path;
	if (!modpath.empty() && modpath.back() != L'\\' && modpath.back() != L'/')
	{
		modpath += L'\\';
	}
	BuildModContentRoots();
}

// A mod can keep a large shared CORE of assets next to its root Data\ that
// the per-submod content layers on top of -- notably Mod, whose `Core` folder
// holds hundreds of loose .alo (e.g. GalloFree_HTT26.alo) shared across its submods
// (Mod/GCW/Rev/TR). The editor only searched the mod ROOT, so all of Core was
// invisible. We add the mod root PLUS its `Core` core folder (if it has a
// Data\Art tree). Root goes first (override content wins), so existing lookups are
// byte-for-byte unchanged -- purely additive.
//
// We deliberately DON'T pull in the per-submod folders: a mod loads ONE submod at a
// time (they cross-contaminate on shared asset names). Selecting a specific submod
// is a separate follow-up.
void FileManager::BuildModContentRoots()
{
	modContentRoots.clear();
	if (modpath.empty()) return;
	modContentRoots.push_back(modpath);

	// The shared core folder (Mod convention: "Core"), if present with content.
	const wstring core = modpath + L"Core\\";
	const DWORD attr = GetFileAttributesW((core + L"Data\\Art").c_str());
	if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
		modContentRoots.push_back(core);
}
