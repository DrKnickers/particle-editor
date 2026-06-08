#include <cassert>
#include <vector>
#include "ChunkFile.h"
#include "exceptions.h"
using namespace std;

ChunkType ChunkReader::nextMini()
{
	assert(m_curDepth >= 0);
	assert(m_size >= 0);

	if (m_miniSize >= 0)
	{
		// We're in a mini chunk, so skip it
		skip();
	}

	if (m_file->tell() == m_offsets[m_curDepth])
	{
		// We're at the end of the current chunk, move up one
		m_curDepth--;
		m_size     = -1;
		m_position =  0;
		return -1;
	}

	MINICHUNKHDR hdr;
	if (m_file->read((void*)&hdr, sizeof(MINICHUNKHDR)) != sizeof(MINICHUNKHDR))
	{
		throw ReadException();
	}

	m_miniSize   = letohl(hdr.size);
	m_miniOffset = m_file->tell() + m_miniSize;
	m_position   = 0;

	return letohl(hdr.type);
}

ChunkType ChunkReader::next()
{
	assert(m_curDepth >= 0);

	if (m_size >= 0)
	{
		// We're in a data chunk, so skip it
		skip();
	}
	
	if (m_file->tell() == m_offsets[m_curDepth])
	{
		// We're at the end of the current chunk, move up one
		m_curDepth--;
		m_size     = -1;
		m_position =  0;
		return -1;
	}

	CHUNKHDR hdr;
	if (m_file->read((void*)&hdr, sizeof(CHUNKHDR)) != sizeof(CHUNKHDR))
	{
		throw ReadException();
	}

	unsigned long size = letohl(hdr.size);
	// Guard the fixed m_offsets[MAX_CHUNK_DEPTH] array: a crafted .alo with
	// chunks nested past depth 255 would otherwise write out of bounds via
	// the pre-increment below (CWE-787 heap corruption during parse).
	// Reject the file. (nextMini() uses the flat m_miniOffset and is not
	// affected.)
	if (m_curDepth + 1 >= MAX_CHUNK_DEPTH)
	{
		throw BadFileException();
	}
	m_offsets[ ++m_curDepth ] = m_file->tell() + (size & 0x7FFFFFFF);
	m_size     = (~size & 0x80000000) ? size : -1;
	m_miniSize = -1;
	m_position = 0;

	return letohl(hdr.type);
}

void ChunkReader::skip()
{
	if (m_miniSize >= 0)
	{
		m_file->seek(m_miniOffset);
	}
	else
	{
		m_file->seek(m_offsets[m_curDepth--]);
	}
}

long ChunkReader::size()
{
	return (m_miniSize >= 0) ? m_miniSize : m_size;
}

string ChunkReader::readString()
{
	// A string chunk stores its bytes plus a trailing NUL (see
	// ChunkWriter::writeString, which writes length()+1 bytes). The old
	// body did `str = data;` -- std::string(const char*) walks to the
	// first NUL, reading past the heap allocation on any malformed or
	// unterminated string chunk (CWE-125 heap over-read); a zero-length
	// chunk made `new char[0]` + C-string assign undefined too. Read into
	// a bounded buffer, require the terminator, and construct the string
	// length-bounded so neither a missing terminator nor an embedded NUL
	// can misbehave.
	const long len = size();
	if (len <= 0)
	{
		throw BadFileException();
	}
	std::vector<char> buf((size_t)len);
	read(buf.data(), len);
	if (buf.back() != '\0')
	{
		throw BadFileException();
	}
	return string(buf.data(), (size_t)len - 1);
}

long ChunkReader::read(void* buffer, long size, bool check)
{
	if (m_size >= 0)
	{
		unsigned long s = m_file->read(buffer, min(m_position + size, this->size()) - m_position);
		m_position += s;
		if (check && s != size)
		{
			throw ReadException();
		}
		return size;
	}
	throw ReadException();
}

ChunkReader::ChunkReader(IFile* file)
{
	file->AddRef();
	m_file       = file;
	m_offsets[0] = m_file->size();
	m_curDepth   = 0;
	m_size       = -1;
	m_miniSize   = -1;
}

ChunkReader::~ChunkReader()
{
	m_file->Release();
}
