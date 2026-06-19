#include <algorithm>
#include <iostream>
#include "managers.h"
#include "exceptions.h"
#include "crc32.h"
#include "xml.h"
#include "utils.h"
#include "ModLayers.h"
using namespace std;

//
// FileManager class
//
IFile* FileManager::getFile(const string& path)
{
	// If a mod is selected, try its content roots first (in precedence order --
	// submods, then Core, then the mod root; see BuildModContentRoots) so mod
	// loose files shadow the base game's. First match wins: the engine REPLACES a
	// file by precedence, never merges, so this single resolved copy is faithful.
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
	submods.clear();   // a new mod has its own submods; reset the stack
	BuildModContentRoots();
}

// Select the ordered submod stack under the active mod (empty to clear) and
// rebuild the content roots. Each submod's Data\Art layers on top of Core, in
// precedence order (front wins).
void FileManager::SetSubmods(const vector<wstring>& names)
{
	submods = names;
	BuildModContentRoots();
}

// A mod can keep a large shared CORE of assets next to its root Data\ that
// the per-submod content layers on top of -- notably Mod, whose `Core` folder
// holds hundreds of loose .alo (e.g. GalloFree_HTT26.alo) shared across its submods
// (Mod/GCW/Rev/TR). The editor only searched the mod ROOT, so all of Core was
// invisible. We add the mod root PLUS its `Core` core folder (if it has a
// Data\Art tree). The mod root is the LOWEST-precedence mod layer (see the
// ordering note below) -- it does NOT shadow a submod/Core copy of the same file.
//
// A mod can stack several submods explicitly, in precedence order. The
// order matches Mod's own launch parameters (LEFT = highest), where the mod root is
// the LOWEST mod layer -- a stale file in the root must NOT shadow a submod's copy.
// Search order (first match wins in getFile; the game replaces per file, never merges):
//   submods[0..n] (the selected stack, front = highest precedence; each needs a Data\Art tree)
//   mod root      (lowest mod layer; the game lists it last)
//   ...base game  (appended later in getFile)
// Core is just another entry in `submods` now -- the user selects + orders it
// in the Submods dialog (it was previously auto-appended here, which wrongly forced it on
// for Mod's Rev config; ModManager migrates legacy selections to keep it for Mod/IR/TR).
void FileManager::BuildModContentRoots()
{
	modContentRoots.clear();
	if (modpath.empty()) return;

	auto hasArtTree = [](const wstring& root) -> bool {
		const DWORD attr = GetFileAttributesW((root + L"Data\\Art").c_str());
		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
	};

	// FAITHFUL precedence, matching Mod's launch parameters (a submod ships a
	// chain like `Modpath=...\Mod Modpath=...\Core Modpath=...`, LEFT = highest):
	// the selected submod stack first (front = highest, Core among them where the
	// user placed it), then the MOD ROOT LAST. The earlier mod-root-FIRST order was
	// inverted -- it let a stale file in the mod root (e.g. its old HardPointDataFiles.xml)
	// shadow the active submod's real one, which the game replaces the other way round.
	// The game REPLACES per file by precedence (never merges), so getFile's first-match
	// is faithful once the root order is right.
	for (const wstring& sub : submods)
	{
		if (sub.empty()) continue;
		const wstring subRoot = modpath + sub + L"\\";
		if (hasArtTree(subRoot))
			modContentRoots.push_back(subRoot);
	}

	// The mod root is the LOWEST-precedence mod layer (the game lists it last).
	modContentRoots.push_back(modpath);
}

// Stack-aware content-root setter (see managers.h). Delegates the
// canonicalize + existence-filter + dedup + slash-terminate logic to the pure
// modlayers::BuildContentRoots, supplying a real directory-existence predicate.
void FileManager::SetLayers(const vector<wstring>& absoluteLayers)
{
	modContentRoots = modlayers::BuildContentRoots(absoluteLayers,
		[](const wstring& dir) -> bool {
			const DWORD a = GetFileAttributesW(dir.c_str());
			return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
		});
}
