AM_CPPFLAGS = -I$(top_srcdir)/src \
	      -I$(top_srcdir)/src/glbindings

#if WITH_MATH3D
AM_CPPFLAGS += -I$(top_srcdir)/src/math
#endif

#if WITH_TEXT
AM_CPPFLAGS += -I$(top_srcdir)/src/text
#endif

lib_LTLIBRARIES = libglplatform.la
libglplatform_la_LIBADD=$(FREETYPE2_LIBS)
libglplatform_la_CFLAGS=-std=c99 $(FREETYPE2_CFLAGS)
libglplatform_la_SOURCES = src/glbindings/glplatform-glcore.c

if WINDOWS
libglplatform_la_SOURCES += src/glplatform-win32.c \
			   src/glbindings/glplatform-wgl.c
endif

if LINUX_GNU
libglplatform_la_SOURCES += src/glplatform-linux.c \
			   src/glbindings/glplatform-glx.c
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

if WINDOWS
WINDOWS_LDADD = -lkernel32 -luser32 -lopengl32 -lgdi32
endif

simple_window_SOURCES = src/examples/simple_window.c
simple_window_LDADD = libglplatform.la

if WINDOWS
simple_window_LDADD += $(WINDOWS_LDADD)
endif


text_render_SOURCES = src/examples/text_render.c
text_render_CFLAGS = -DTTF_PATH=\"${abs_srcdir}/src/examples/ttf/\"
text_render_LDADD = libglplatform.la

if WINDOWS
text_render_LDADD += $(WINDOWS_LDADD)
endif

pkginclude_HEADERS = src/glbindings/glplatform-glcore.h \
		     src/glbindings/glplatform-glx.h \
		     src/glplatform.h

if WITH_MATH3D
pkginclude_HEADERS += src/math/math3d.h
endif

if WITH_TEXT
pkginclude_HEADERS += src/text/gltext.h
endif
