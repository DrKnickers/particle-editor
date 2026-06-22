#ifndef UTILS_H
#define UTILS_H

#include <string>

// Convert an ANSI string to a wide (UCS-2) string
std::wstring AnsiToWide(const char* cstr);
static std::wstring AnsiToWide(const std::string& str)
{
	return AnsiToWide(str.c_str());
}

// Convert  a wide (UCS-2) string to an an ANSI string
std::string WideToAnsi(const wchar_t* cstr,     const char* defChar = " ");
static std::string WideToAnsi(const std::wstring& str, const char* defChar = " ")
{
	return WideToAnsi(str.c_str(), defChar);
}

float GetRandom(float min, float max);

std::wstring LoadString(UINT id, ...);

#endif