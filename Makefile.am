ACLOCAL_AMFLAGS=-I m4

AM_CPPFLAGS = -I$(top_srcdir)/src \
	      -I$(top_srcdir)/src/glbindings

#if WITH_MATH3D
AM_CPPFLAGS += -I$(top_srcdir)/src/math
#endif

#if WITH_TEXT
AM_CPPFLAGS += -I$(top_srcdir)/src/text
#endif

AM_CFLAGS = -std=c11

lib_LTLIBRARIES = libglplatform.la
libglplatform_la_LIBADD=$(FREETYPE2_LIBS)
libglplatform_la_CFLAGS=$(FREETYPE2_CFLAGS) $(AM_CFLAGS)
libglplatform_la_LDFLAGS = -version-info 4:0:0 -no-undefined
libglplatform_la_SOURCES = src/glbindings/glcore.c

if WINDOWS
libglplatform_la_SOURCES += src/win32.c src/glbindings/wgl.c
libglplatform_la_LIBADD += -lkernel32 -luser32 -lopengl32 -lgdi32
libglplatform_la_CFLAGS += -DGLPLATFORM_ENABLE_WGL_ARB_create_context \
			-DGLPLATFORM_ENABLE_WGL_ARB_create_context_profile \
			-DGLPLATFORM_ENABLE_WGL_ARB_make_current_read
endif

if LINUX_GNU
libglplatform_la_SOURCES += src/linux.c src/glbindings/glx.c
libglplatform_la_CFLAGS += -DGLPLATFORM_ENABLE_GLX_ARB_create_context \
			-DGLPLATFORM_ENABLE_GLX_ARB_create_context_profile
endif

if WITH_MATH3D
libglplatform_la_SOURCES += src/math/math3d.c
endif

if WITH_TEXT
libglplatform_la_SOURCES += src/text/gltext.c
endif

noinst_PROGRAMS = simple_window
if WITH_TEXT
noinst_PROGRAMS += text_render
endif

simple_window_SOURCES = src/examples/simple_window.c
simple_window_LDADD = libglplatform.la
simple_window_CFLAGS = $(AM_CFLAGS)

text_render_SOURCES = src/examples/text_render.c
text_render_CFLAGS = -DTTF_PATH=\"${abs_srcdir}/src/examples/ttf/\" $(AM_CFLAGS)
text_render_LDADD = libglplatform.la

pkginclude_HEADERS = src/glbindings/glcore.h \
		     src/glplatform.h

if LINUX_GNU
pkginclude_HEADERS += src/glbindings/glx.h
endif

if WINDOWS
pkginclude_HEADERS += src/glbindings/wgl.h
endif

if WITH_MATH3D
pkginclude_HEADERS += src/math/math3d.h
endif

if WITH_TEXT
pkginclude_HEADERS += src/text/gltext.h
endif
