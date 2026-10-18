#ifndef _CONFIG_H
#define _CONFIG_H

/* Define to the version of this package. */
#define PACKAGE_VERSION "${PROJECT_VERSION}"

/* major version number */
#define MODULE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR}

/* minor version number */
#define MODULE_VERSION_MINOR ${PROJECT_VERSION_MINOR}

/* sub version number */
#define MODULE_VERSION_SUB ${PROJECT_VERSION_PATCH}

/* Define if compiling with gcc visibility support */
#cmakedefine HAVE_GCC_VISIBILITY

/* Define if qore supports ports in the Datasource object */
#cmakedefine QORE_HAS_DATASOURCE_PORT

/* Define if Datasource::activeTransaction() is available */
#cmakedefine HAVE_QORE_DATASOURCE_ACTIVE_TRANSACTION

/* Define if QDBI_METHOD_EXECRAW is available; defined to 1, as qore/QoreLib.h defines it in Qore versions that
   have it, so that the definitions are identical */
#cmakedefine _QORE_HAS_DBI_EXECRAW 1

/* Define if Qore has columnar DBI result APIs */
#cmakedefine HAVE_QORE_COLUMNAR_RESULT

/* Define if Qore has the native bulk-load DBI APIs */
#cmakedefine HAVE_QORE_BULK_LOAD

/* Define if FreeTDS provides the CT-Library bulk APIs */
#cmakedefine HAVE_FREETDS_BULK

/* Define if freetds ct-lib cs_loc_alloc, cs_locale are implemented */
#cmakedefine FREETDS_LOCALE

/* Define if this is a 64-bit compile */
#cmakedefine SYB_LP64

/* Define if debugging support should be included */
#ifndef DEBUG
#cmakedefine DEBUG
#endif

/* Define if assert() declarations should be suppressed */
#ifndef NDEBUG
#cmakedefine NDEBUG
#endif

/* Host architecture */
#define MODULE_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}"

/* Host OS */
#define MODULE_TARGET_OS "${CMAKE_SYSTEM_NAME}"

/* 32 or 64 bit build */
#cmakedefine MODULE_TARGET_BITS @MODULE_TARGET_BITS@

/* Define if compiling on Linux */
#cmakedefine LINUX

/* Define if compiling on Darwin */
#cmakedefine DARWIN

/* Define if compiling on Solaris */
#cmakedefine SOLARIS

/* Define if compiling on HP-UX */
#cmakedefine HPUX

#endif /* _CONFIG_H */
