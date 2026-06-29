#include <algorithm>
#include <iostream>
#include "MegaFiles.h"
#include "exceptions.h"
#include "crc32.h"
#include "xml.h"
#include "ResourceLimits.h"
using namespace std;

//
// MegaFile class
//
MegaFile::MegaFile(IFile* file)
{
	this->file = file;
	this->file->AddRef();

	try
	{
		//
		// Read sizes
		//
		uint32_t numStrings;
		uint32_t numFiles;
		if (file->read((void*)&numStrings, sizeof(uint32_t)) != sizeof(uint32_t) ||
			file->read((void*)&numFiles, sizeof(uint32_t)) != sizeof(uint32_t))
		{
			throw ReadException();
		}

		// Cap the counts against the file size BEFORE the read loops
		// (each filename needs >= 2 bytes; each FileInfo is sizeof(FileInfo)).
		// A forged huge count would otherwise drive an OOM allocation loop and
		// overflow the start/totalsize math below.
		const unsigned long fsize = (unsigned long)file->size();
		if (numStrings > kMaxMegEntryCount ||
			numFiles > kMaxMegEntryCount ||
			numStrings > fsize / 2 || numFiles > fsize / sizeof(FileInfo))
		{
			throw BadFileException();
		}

		//
		// Read filenames
		//
		unsigned long runningNameBytes = 0;
		for (unsigned long i = 0; i < numStrings; i++)
		{
			uint16_t length;
			if (file->read((void*)&length, sizeof(uint16_t)) != sizeof(uint16_t))
			{
				throw ReadException();
			}
			const unsigned long entryBytes = (unsigned long)sizeof(uint16_t) + length;
			if (length > kMaxFilenameLength ||
				runningNameBytes > kMaxMegNameTableBytes - entryBytes)
			{
				throw BadFileException();
			}
			runningNameBytes += entryBytes;

			char* data = new char[length + 1];
			if (file->read(data, length) != length)
			{
				delete[] data;
				throw ReadException();
			}
			data[length] = '\0';
			filenames.push_back(data);
			delete[] data;
		}

		//
		// Read master index table
		//
		unsigned long start = file->tell() + numFiles * sizeof(FileInfo);
		unsigned long totalsize = file->size() - start;

		for (unsigned long i = 0; i < numFiles; i++)
		{
			FileInfo info;
			if (file->read((void*)&info, sizeof(FileInfo)) != sizeof(FileInfo))
			{
				throw ReadException();
			}
			// A forged nameIndex makes getFile() index filenames[]
			// out of bounds (std::vector::operator[] is UB, NOT a throw the
			// catch below would catch). Validate every entry up front so every
			// files[*] is safe before any lookup. Bound start/size too.
			if (info.nameIndex >= numStrings ||
				info.start > fsize || info.size > fsize - info.start)
			{
				throw BadFileException();
			}
			files.push_back(info);
		}
	}
	catch (IOException&)
	{
		throw BadFileException();
	}
}

MegaFile::~MegaFile()
{
	file->Release();
}

IFile* MegaFile::getFile(std::string path) const
{
	try
	{
		transform(path.begin(), path.end(), path.begin(), toupper);
		unsigned long crc = crc32(path.c_str(), path.size());

		// Do a binary search (entries are sorted by CRC32)
		int last = (int)files.size() - 1;
		int low = 0, high = last;
		while (high >= low)
		{
			int mid = (low + high) / 2;
			if (files[mid].crc == crc)
			{
				// Found a match; find all adjacent matches
				high = low = mid;
				while (low  > 0 && files[low - 1].crc == crc) low--;
				while (high < last && files[high + 1].crc == crc) high++;
				for (mid = low; mid <= high; mid++)
				{
					if (filenames[files[mid].nameIndex] == path)
					{
						return new SubFile(file, files[mid].start, files[mid].size);
					}
				}
				break;
			}
			if (crc < files[mid].crc) high = mid - 1;
			else                      low = mid + 1;
		}
	}
	catch (IOException&)
	{
		throw BadFileException();
	}
	return NULL;
}
