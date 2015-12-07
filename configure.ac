AC_PREREQ(2.59)
AC_INIT(glplatform, 0.3)

AC_CONFIG_MACRO_DIRS([m4])

AC_CANONICAL_SYSTEM

AM_INIT_AUTOMAKE([foreign subdir-objects])

AC_ARG_ENABLE([text], AS_HELP_STRING([--enable-text], [Enables text rendering API]))
AC_ARG_ENABLE([math3d], AS_HELP_STRING([--enable-math3d], [Enables 3D mathematics API]))

if test x$enable_text = x; then
	enable_text="yes"
fi
if test x$enable_math3d = x; then
	enable_math3d="yes"
fi

AS_IF([ test $enable_text = yes ],
	[PKG_CHECK_MODULES(FREETYPE2,freetype2 >= 2.53, [ enable_text="yes" ], [ enable_text="no" ])])

AC_CHECK_HEADER("iconv.h",,AC_MSG_ERROR([Could not find iconv.h]))

AS_IF([ test $host_os = 'linux-gnu' ],
	[AC_CHECK_LIB([X11],[XOpenDisplay],,AC_MSG_ERROR([Could not find libX11]))
	 AC_CHECK_LIB([GL],[glXGetProcAddress],,AC_MSG_ERROR([Could not find libGL]))])

AM_CONDITIONAL([WITH_TEXT], [ test $enable_text = yes ])
AM_CONDITIONAL([WITH_MATH3D], [ test $enable_math3d = yes ])
AM_CONDITIONAL([WINDOWS], [ test $host_os = mingw32 ])
AM_CONDITIONAL([LINUX_GNU], [ test $host_os = linux-gnu ])

LT_INIT()

AC_PROG_CC

AC_CONFIG_FILES(Makefile)
AC_OUTPUT
