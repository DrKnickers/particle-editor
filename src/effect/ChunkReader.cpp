#include <vector>
#include "ChunkFile.h"
#include "common/exceptions.h"
#include "common/ResourceLimits.h"
using namespace std;

ChunkType ChunkReader::nextMini()
{
	// Mini-chunks live only inside a data chunk. These used to be asserts,
	// gone in Release: past the end of the file m_curDepth is -1 and
	// m_offsets[-1] was read below; inside a container (m_size < 0) the
	// container's bytes were parsed as mini-chunks.
	if (m_curDepth < 0 || m_size < 0)
	{
		throw BadFileException();
	}

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

	// Validate the untrusted mini-chunk size against the parent's remaining bytes
	// AND an absolute cap before trusting it for offset math / later allocations.
	const long miniSize = (long)letohl(hdr.size);
	const long avail    = m_offsets[m_curDepth] - (long)m_file->tell();
	if (miniSize < 0 || miniSize > avail || (unsigned long)miniSize > kMaxAloChunkBytes)
	{
		throw BadFileException();
	}
	m_miniSize   = miniSize;
	m_miniOffset = m_file->tell() + miniSize;
	m_position   = 0;

	return letohl(hdr.type);
}

ChunkType ChunkReader::next()
{
	// Past the end of the file (the top level already returned -1)
	if (m_curDepth < 0)
	{
		throw BadFileException();
	}

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

	// ChunkType is a signed long, so an on-disk type 0xFFFFFFFF would be
	// returned as -1, the end-of-chunk sentinel, after we've already
	// descended into it: the caller and the reader then disagree on depth.
	if (letohl(hdr.type) == 0xFFFFFFFF)
	{
		throw BadFileException();
	}

	unsigned long size = letohl(hdr.size);
	const long payloadSize = (long)(size & 0x7FFFFFFF);
	const long parentRemaining = m_offsets[m_curDepth] - (long)m_file->tell();
	// Parent-relative bound AND the absolute kMaxAloChunkBytes cap. The cap
	// previously guarded only mini-chunks (nextMini); a normal
	// chunk's only bound was its parent, and the top-level parent is the whole
	// file — so a multi-GiB crafted .alo could still authorize a huge single
	// chunk. Real chunks sit far below the cap.
	if (parentRemaining < 0 || payloadSize > parentRemaining ||
	    (unsigned long)payloadSize > kMaxAloChunkBytes)
	{
		throw BadFileException();
	}
	// Guard the fixed m_offsets[MAX_CHUNK_DEPTH] array: a crafted .alo with
	// chunks nested past depth 255 would otherwise write out of bounds via
	// the pre-increment below (CWE-787 heap corruption during parse).
	// Reject the file. (nextMini() uses the flat m_miniOffset and is not
	// affected.)
	if (m_curDepth + 1 >= MAX_CHUNK_DEPTH)
	{
		throw BadFileException();
	}
	m_offsets[ ++m_curDepth ] = m_file->tell() + payloadSize;
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
		// Skipping pops one level; there is no chunk to skip at the top
		// level (or past the end of the file), and popping there used to
		// index m_offsets[-1].
		if (m_curDepth <= 0)
		{
			throw BadFileException();
		}
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
	if ((unsigned long)len > kMaxAloStringBytes) // absolute cap
	{
		throw BadFileException();
	}
	const long end = (m_miniSize >= 0) ? m_miniOffset : m_offsets[m_curDepth];
	const long remaining = end - (long)m_file->tell();
	if (remaining < 0 || len > remaining)
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

long ChunkReader::read(void* buffer, long size)
{
	if (m_size >= 0)
	{
		unsigned long s = m_file->read(buffer, min(m_position + size, this->size()) - m_position);
		m_position += s;
		if (s != size)
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
	m_position   = 0;
	m_miniOffset = 0;
}

ChunkReader::~ChunkReader()
{
	m_file->Release();
}
