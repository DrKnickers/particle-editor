#include <cassert>
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
	// Depth guard. Pre-fix m_curDepth was incremented unchecked and the
	// write to m_offsets[m_curDepth] could scribble past the fixed
	// array — a crafted .alo with excessive nesting would corrupt
	// adjacent memory during parse. (F3.)
	if (m_curDepth + 1 >= MAX_CHUNK_DEPTH) throw ReadException();
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
	// Length-bounded read. Pre-fix this allocated `new char[size()]`,
	// read exactly `size()` bytes, then did `str = data` — which
	// invokes std::string::operator=(const char*) and walks past the
	// allocation until it finds a zero byte if the chunk omits the
	// terminator. (F2.) Size the std::string to the chunk
	// byte count, read directly into its storage, then trim at the
	// first NUL if present (chunk format may or may not include one).
	long s = size();
	if (s < 0) throw ReadException();
	if (s == 0) return string();
	string str((size_t)s, '\0');
	read(&str[0], s);
	size_t nulPos = str.find('\0');
	if (nulPos != string::npos)
	{
		str.resize(nulPos);
	}
	return str;
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
