AUTOMAKE_OPTIONS = foreign

SUBDIRS = src

noinst_HEADERS = src/bulk_load.h \
	src/command.h \
	src/common_constants.h \
	src/connection.h \
	src/conversions.h \
	src/dbmodulewrap.h \
	src/emptystatement.h \
	src/encoding_helpers.h \
	src/error.h \
	src/resultfactory.h \
	src/row_output_buffers.h \
	src/sybase.h \
	src/sybase_query.h \
	src/utils.h \
	src/resultfactory.h \
	src/row_output_buffers.h \
	src/statement.h

EXTRA_DIST = COPYING.MIT COPYING.LGPL AUTHORS README \
	RELEASE-NOTES \
	test/freetds-native-bulk-load.qtest \
	test/sybase-statement.qtest \
	test/sybase-types.qtest \
	qore-sybase-modules.spec

ACLOCAL_AMFLAGS=-I m4

if COND_DOXYGEN
DOX = sybase
DOXYGEN_OUTPUT = docs/sybase/html
DOXYFILES = docs/doxyfile docs/mainpage.dox
DOXYFILES += docs/guide-sybasereleasenotes.dox docs/guide-sybasestoredprocs.dox docs/guide-sybasebind.dox docs/guide-sybase_timezone.dox docs/guide-freetds_native_bulk_load.dox docs/guide-sybaseoptions.dox
DOXYFILES_SRC = docs/doxyfile.tmpl docs/footer_template.html
QDX = qdx
DX_CLEANFILES = ${DOXYFILES}

docs/sybase/html: $(DOXYFILES)
	cd docs; $(DOXYGEN_CMD) doxyfile;
	$(QDX) --post docs/sybase/html/*html
	$(QDX) --post docs/sybase/html/search/*html

docs/mainpage.dox: docs/mainpage.dox.tmpl
	$(QDX) -d $< $@

docs/doxyfile: docs/doxyfile.tmpl
	sed -e s/__VERSION__/${VERSION}/ -e s:__USERMODDIR__:${usermoddir}: $< > $@

docs/guide-sybasereleasenotes.dox: docs/guide-sybasereleasenotes.dox.tmpl
	$(QDX) -d $< $@

docs/guide-sybasestoredprocs.dox: docs/guide-sybasestoredprocs.dox.tmpl
	$(QDX) -d $< $@

docs/guide-sybasebind.dox: docs/guide-sybasebind.dox.tmpl
	$(QDX) -d $< $@

docs/guide-sybase_timezone.dox: docs/guide-sybase_timezone.dox.tmpl
	$(QDX) -d $< $@

docs/guide-freetds_native_bulk_load.dox: docs/guide-freetds_native_bulk_load.dox.tmpl
	$(QDX) -d $< $@

docs/guide-sybaseoptions.dox: docs/guide-sybaseoptions.dox.tmpl
	$(QDX) -d $< $@


doxygen-doc: $(DOXYGEN_OUTPUT)

EXTRA_DIST += $(DOXYGEN_OUTPUT)

html-local: $(DOXYGEN_OUTPUT)

install-html-local:
	for dox in $(DOX); do $(MKDIR_P) $(DESTDIR)$(datadir)/$(PACKAGE)/docs/$$dox/html/search; done
	for dir in $(DOXYGEN_OUTPUT); do \
	   for f in $$dir/*; do if [ -f $$f ]; then $(INSTALL_DATA) $$f $(DESTDIR)$(datadir)/$(PACKAGE)/$$dir; fi; done; \
	   for f in $$dir/search/*; do $(INSTALL_DATA) $$f $(DESTDIR)$(datadir)/$(PACKAGE)/$$dir/search; done; \
	done

clean-local:
	$(RM) -rf $(DOXYGEN_OUTPUT)

uninstall-local:
	$(RM) -rf $(DESTDIR)$(datadir)/$(PACKAGE)/docs

mostlyclean-local:
	rm -rf ${DOXYGEN_OUTPUT} ${DX_CLEANFILES}
else
doxygen-doc:
	echo no doxygen support
endif

libtool: $(LIBTOOL_DEPS)
	$(SHELL) ./config.status --recheck

# Documentation guide sources shipped with the module.
EXTRA_DIST += docs/guide-sybasereleasenotes.dox.tmpl \
	docs/guide-sybasestoredprocs.dox.tmpl \
	docs/guide-sybasebind.dox.tmpl \
	docs/guide-sybase_timezone.dox.tmpl \
	docs/guide-freetds_native_bulk_load.dox.tmpl \
	docs/guide-sybaseoptions.dox.tmpl
