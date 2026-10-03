#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include "types.h"
#include "utils.h"
#include "Resources/resource.h"
#include <string>
#include <stdexcept>

class wexception : public std::exception
{
	std::wstring message;
	std::string  narrowMessage;

	// ASCII copy for code that only knows std::exception (non-ASCII becomes
	// '?'), so it is valid as both ANSI and UTF-8
	void narrow()
	{
		narrowMessage.reserve(message.size());
		for (size_t i = 0; i < message.size(); i++)
		{
			narrowMessage += (message[i] < 0x80) ? (char)message[i] : '?';
		}
	}

public:
	const wchar_t* wwhat() const { return message.c_str(); }
	const char*    what() const noexcept override { return narrowMessage.c_str(); }
	wexception(const wchar_t* _message) : message(_message) { narrow(); }
	wexception(const std::wstring& _message) : message(_message) { narrow(); }
};

class wruntime_error : public wexception
{
public:
	wruntime_error(const std::wstring& message) : wexception(message) {}
};

class IOException : public wruntime_error
{
public:
	IOException(const std::wstring& message) : wruntime_error(message) {}
};

class ParseException : public wruntime_error
{
public:
	ParseException(const std::wstring& message) : wruntime_error(message) {}
};

class FileNotFoundException : public IOException
{
public:
	FileNotFoundException(const std::wstring filename)
        : IOException(LoadString(IDS_ERROR_FILE_FIND, filename.c_str())) {}
};

class ReadException : public IOException
{
public:
	ReadException() : IOException(LoadString(IDS_ERROR_FILE_READ)) {}
};

class WriteException : public IOException
{
public:
	WriteException() : IOException(LoadString(IDS_ERROR_FILE_WRITE)) {}
};

class BadFileException : public IOException
{
public:
	BadFileException(const std::wstring message) : IOException(message) {}
	BadFileException() : IOException(LoadString(IDS_ERROR_FILE_CORRUPT)) {}
};

class WrongFileException : public BadFileException
{
public:
	WrongFileException() : BadFileException(LoadString(IDS_ERROR_FILE_FORMAT)) {}
};

#endif