AC_PREREQ(2.59)
AC_INIT(glplatform, [0.4])

AC_CONFIG_MACRO_DIRS([m4])

AC_CANONICAL_SYSTEM

AM_INIT_AUTOMAKE([foreign subdir-objects])

PKG_CHECK_MODULES(FREETYPE2,freetype2 >= 2.53,,AC_MSG_ERROR([Could not find libX11]))

AS_IF([ test $host_os = 'linux-gnu' ],
	[AC_CHECK_LIB([X11],[XOpenDisplay],,AC_MSG_ERROR([Could not find libX11]))
	 AC_CHECK_LIB([GL],[glXGetProcAddress],,AC_MSG_ERROR([Could not find libGL]))])

AM_CONDITIONAL([WINDOWS], [ test $host_os = mingw32 ])
AM_CONDITIONAL([LINUX_GNU], [ test $host_os = linux-gnu ])

LT_INIT()

AC_PROG_CC

AC_CONFIG_FILES(Makefile)
AC_OUTPUT
