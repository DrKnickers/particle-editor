/* expat_config.h for the Particle Editor's MSVC x64 build of Expat 2.8.5.

   NOT an upstream file. Upstream generates this header at configure time
   (expat_config.h.cmake via CMake, expat_config.h.in via autotools); the
   release tarball's pre-generated expat_config.h is a Linux configure result
   (arc4random / getrandom / unistd.h ...) and must not be used on Windows.
   This is what expat_config.h.cmake expands to under MSVC with upstream's
   CMake defaults: EXPAT_DTD=ON, EXPAT_GE=ON, EXPAT_NS=ON,
   EXPAT_CONTEXT_BYTES=1024, EXPAT_ATTR_INFO=OFF, EXPAT_LARGE_SIZE=OFF.

   Entropy: no HAVE_ARC4RANDOM* / HAVE_GETRANDOM / HAVE_GETENTROPY /
   XML_DEV_URANDOM, so xmlparse.c takes its _WIN32 path and salts the hash
   tables with rand_s() (lib/random_rand_s.c). */

#ifndef EXPAT_CONFIG_H
#  define EXPAT_CONFIG_H 1

/* 1234 = LIL_ENDIAN, 4321 = BIGENDIAN */
#  define BYTEORDER 1234

/* Headers the MSVC CRT provides (CMake check_include_file results). */
#  define HAVE_FCNTL_H 1
#  define HAVE_INTTYPES_H 1
#  define HAVE_MEMORY_H 1
#  define HAVE_STDINT_H 1
#  define HAVE_STDLIB_H 1
#  define HAVE_STRING_H 1
#  define HAVE_SYS_STAT_H 1
#  define HAVE_SYS_TYPES_H 1

#  define PACKAGE "expat"
#  define PACKAGE_BUGREPORT "https://github.com/libexpat/libexpat/issues"
#  define PACKAGE_NAME "expat"
#  define PACKAGE_STRING "expat 2.8.5"
#  define PACKAGE_TARNAME "expat"
#  define PACKAGE_URL ""
#  define PACKAGE_VERSION "2.8.5"

#  ifndef STDC_HEADERS
#    define STDC_HEADERS 1
#  endif

/* Define to specify how much context to retain around the current parse
   point, 0 to disable. */
#  define XML_CONTEXT_BYTES 1024

/* Define to make parameter entity parsing functionality available. */
#  define XML_DTD 1

/* Define as 1/0 to enable/disable support for general entities. */
#  define XML_GE 1

/* Define to make XML Namespaces functionality available. */
#  define XML_NS 1

#  ifdef _MSC_VER
#    define __func__ __FUNCTION__
#  endif

#endif // ndef EXPAT_CONFIG_H
