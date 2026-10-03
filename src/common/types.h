#ifndef TYPES_H
#define TYPES_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <stdint.h>

// Little-endian load/store helpers. The byte-swap form is correct on any
// endianness; the build is x64-only, and only the 32-bit pair is used.
inline uint32_t letohl(uint32_t value)
{
	return ((uint32_t)((uint8_t*)&value)[3] << 24) | ((uint32_t)((uint8_t*)&value)[2] << 16)|
	       ((uint32_t)((uint8_t*)&value)[1] <<  8) | ((uint32_t)((uint8_t*)&value)[0] <<  0);
}

inline uint32_t htolel(uint32_t value)
{
	uint32_t tmp;
	((uint8_t*)&tmp)[0] = (uint8_t)(value >>  0);
	((uint8_t*)&tmp)[1] = (uint8_t)(value >>  8);
	((uint8_t*)&tmp)[2] = (uint8_t)(value >> 16);
	((uint8_t*)&tmp)[3] = (uint8_t)(value >> 24);
	return tmp;
}

class RefCounted
{
	unsigned long references;
protected:
	virtual ~RefCounted() {};
public:
	RefCounted() : references(1) {}
	void AddRef()  { references++; }
	void Release() { if (--references == 0) delete this; }
};

#ifndef SAFE_RELEASE
template <typename T>
void SAFE_RELEASE(T* &p)
{
	if (p != NULL)
	{
		p->Release();
		p = NULL;
	}
}
#endif

#endif