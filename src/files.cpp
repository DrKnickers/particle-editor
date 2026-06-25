#include "files.h"
#include "exceptions.h"
#include <vector>
using namespace std;

// Read every byte of `file` into a freshly-allocated buffer, Release
// the file reference, return the bytes. Throws ReadException on partial
// read or empty file (Releases before throwing). Takes ownership of the
// IFile* reference.
vector<unsigned char> ReadAndRelease(IFile* file)
{
	if (file == NULL) throw ReadException();
	const unsigned long size = file->size();
	if (size == 0) { file->Release(); throw ReadException(); }
	vector<unsigned char> bytes(size);
	const unsigned long got = file->read(bytes.data(), size);
	file->Release();
	if (got != size) throw ReadException();
	return bytes;
}

//
// PhysicalFile
//
unsigned long PhysicalFile::read(void* buffer, unsigned long size)
{
	SetFilePointer(hFile, m_position, NULL, FILE_BEGIN);
	if (!ReadFile(hFile, buffer, size, &size, NULL))
	{
		throw ReadException();
	}
	m_position = min(m_position + size, m_size);
	return size;
}

unsigned long PhysicalFile::write(const void* buffer, unsigned long size)
{
	SetFilePointer(hFile, m_position, NULL, FILE_BEGIN);
	if (!WriteFile(hFile, buffer, size, &size, NULL))
	{
		throw WriteException();
	}
	m_size     = GetFileSize(hFile, NULL);
	m_position = min(m_position + size, m_size);
	return size;
}

PhysicalFile::PhysicalFile(const wstring& filename, Mode mode)
{
	DWORD dwDesiredAccess       = (mode == WRITE ? GENERIC_WRITE : GENERIC_READ);
	DWORD dwCreationDisposition = (mode == WRITE ? CREATE_ALWAYS : OPEN_EXISTING);
	hFile = CreateFile(filename.c_str(), dwDesiredAccess, FILE_SHARE_READ, NULL, dwCreationDisposition, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		DWORD error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
		{
			throw FileNotFoundException(filename);
		}
		if (mode == WRITE)
		{
            throw IOException(LoadString(IDS_ERROR_FILE_CREATE, filename.c_str()));
		}
        throw IOException(LoadString(IDS_ERROR_FILE_OPEN, filename.c_str()));
	}
	m_size     = GetFileSize(hFile, NULL);
	m_position = 0;
}

PhysicalFile::~PhysicalFile()
{
	CloseHandle(hFile);
}

//
// SubFile
//
unsigned long SubFile::read(void* buffer, unsigned long size)
{
	// Clamp the request to the bytes remaining INSIDE this sub-view. Without
	// this, a read larger than the sub-file (e.g. XMLTree::parse's 32 KB chunks
	// on a small packed XML) spills into the adjacent MEG entry's bytes, handing
	// trailing garbage to the caller -- which broke MEG-packed XML parsing
	// (skydome lists resolved but failed to parse). Exact-size reads
	// (ReadAndRelease) are unaffected: size already equals the remaining bytes.
	const unsigned long remaining = (m_position < m_size) ? (m_size - m_position) : 0;
	if (size > remaining) size = remaining;
	if (size == 0) return 0;
	m_file->seek(m_start + m_position);
	size = m_file->read(buffer, size);
	m_position = min(m_position + size, m_size);
	return size;
}

unsigned long SubFile::write(const void* buffer, unsigned long size)
{
	m_file->seek(m_start + m_position);
	size = m_file->write(buffer, size);
	m_size = m_file->size();
	m_position = min(m_position + size, m_size);
	return size;
}

SubFile::SubFile(IFile* file, unsigned long start, unsigned long size)
{
	m_file     = file;
	m_size     = size;
	m_start    = start;
	m_position = 0;

	m_file->AddRef();
}

SubFile::~SubFile()
{
	m_file->Release();
}

//
// MemoryFile
//
unsigned long MemoryFile::read(void* buffer, unsigned long size)
{
	// `m_size - m_position` underflows (both unsigned) if m_position
	// somehow exceeds m_size, yielding a huge clamp and an OOB memcpy. Guard
	// the remaining-bytes computation, mirroring SubFile::read above.
	const unsigned long remaining = (m_position < m_size) ? (m_size - m_position) : 0;
	if (size > remaining) size = remaining;
	if (size == 0) return 0;
    memcpy(buffer, &m_data[m_position], size);
	m_position = m_position + size;
	return size;
}

unsigned long MemoryFile::write(const void* buffer, unsigned long size)
{
    if (m_position + size > m_size)
    {
        // Expand buffer
        char* temp = (char*)realloc(m_data, m_position + size);
        if (temp == NULL)
        {
            throw bad_alloc();
        }
        m_data = temp;
        m_size = m_position + size;
    }
    memcpy(&m_data[m_position], buffer, size);
	m_position = m_position + size;
	return size;
}

MemoryFile::MemoryFile()
{
    m_data     = NULL;
    m_size     = 0;
    m_position = 0;
}

MemoryFile::~MemoryFile()
{
	free(m_data);
}