if COND_SINGLE_COMPILATION_UNIT
SYBASE_SOURCES = single-compilation-unit.cpp
# Explicit dependencies for single-compilation-unit mode
SINGLE_UNIT_DEPS = sybase.cpp connection.cpp conversions.cpp command.cpp \
				   encoding_helpers.cpp sybase_query.cpp row_output_buffers.cpp statement.cpp \
				   sybase.h connection.h conversions.h command.h encoding_helpers.h \
				   sybase_query.h row_output_buffers.h statement.h \
				   resultfactory.h dbmodulewrap.h error.h utils.h common_constants.h \
				   bulk_load.cpp bulk_load.h
single-compilation-unit.cpp: $(SINGLE_UNIT_DEPS)
	@touch $@
else
SYBASE_SOURCES = sybase.cpp connection.cpp\
				 conversions.cpp command.cpp\
				 encoding_helpers.cpp sybase_query.cpp\
				 row_output_buffers.cpp statement.cpp \
				 bulk_load.cpp
endif

lib_LTLIBRARIES =
if COND_SYBASE
lib_LTLIBRARIES += sybase.la
sybase_la_SOURCES = $(SYBASE_SOURCES)
sybase_la_LDFLAGS = -module -avoid-version ${SYBASE_LIBS}
sybase_la_CPPFLAGS = $(QORE_CPPFLAGS) $(SYBASE_INCLUDES) -DSYBASE=1
endif
if COND_FREETDS
lib_LTLIBRARIES += freetds.la
freetds_la_SOURCES = $(SYBASE_SOURCES)
freetds_la_LDFLAGS = -module -avoid-version ${FREETDS_LIBS}
freetds_la_CPPFLAGS = $(QORE_CPPFLAGS) $(FREETDS_INCLUDES) -DFREETDS=1
endif

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_builddir)

if COND_PROFILE
AM_CXXFLAGS=-g -pg
else
if COND_DEBUG
AM_CXXFLAGS=-g
endif
endif

if COND_MINGW
RM_OTHER_SYBASE=$(DESTDIR)$(libdir)/sybase.dll.a
RM_OTHER_FREETDS=$(DESTDIR)$(libdir)/freetds.dll.a
endif

install-exec-hook:
if COND_SYBASE
	mv $(DESTDIR)$(libdir)/sybase.$(MODULE_SUFFIX) $(DESTDIR)$(libdir)/sybase-api-$(MODULE_API).qmod
	rm -f $(DESTDIR)$(libdir)/sybase.la $(DESTDIR)$(libdir)/sybase.a $(RM_OTHER_SYBASE)
endif
if COND_FREETDS
	mv $(DESTDIR)$(libdir)/freetds.$(MODULE_SUFFIX) $(DESTDIR)$(libdir)/freetds-api-$(MODULE_API).qmod
	rm -f $(DESTDIR)$(libdir)/freetds.la $(DESTDIR)$(libdir)/freetds.a $(RM_OTHER_FREETDS)
endif
