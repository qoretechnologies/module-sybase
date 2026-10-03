cmake_minimum_required(VERSION 3.5...3.29)
# Copyright (C) 2026 Qore Technologies, s.r.o.

project(qore-sybase-modules VERSION 1.4)

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)
include(CheckCXXSymbolExists)

find_package(Qore 0.9 REQUIRED)

# Check for C++17
CHECK_CXX_COMPILER_FLAG("-std=c++17" COMPILER_SUPPORTS_CXX17)
if(COMPILER_SUPPORTS_CXX17)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
else()
    message(FATAL_ERROR "The compiler ${CMAKE_CXX_COMPILER} has no C++17 support. Please use a different C++ compiler.")
endif()

set(CMAKE_THREAD_PREFER_PTHREAD ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
if(CMAKE_USE_PTHREADS_INIT)
    message(STATUS "Found POSIX Threads: TRUE")
else(CMAKE_USE_PTHREADS_INIT)
    message(STATUS "Found POSIX Threads: FALSE")
    message(FATAL_ERROR "POSIX threads does not seem to be supported on this platform, aborting")
endif()

# Options for building sybase and freetds modules
option(WITH_SYBASE "Build Sybase OCS module" ON)
option(WITH_FREETDS "Build FreeTDS module" ON)

# Find libraries
if(WITH_SYBASE)
    find_package(Sybase)
    if(NOT Sybase_FOUND)
        message(STATUS "Sybase OCS not found, disabling sybase module")
        set(WITH_SYBASE OFF)
    endif()
endif()

if(WITH_FREETDS)
    find_package(FreeTDS)
    if(NOT FreeTDS_FOUND)
        message(STATUS "FreeTDS not found, disabling freetds module")
        set(WITH_FREETDS OFF)
    endif()
endif()

if(NOT WITH_SYBASE AND NOT WITH_FREETDS)
    message(FATAL_ERROR "Neither Sybase OCS nor FreeTDS found, cannot build any modules")
endif()

# Check for Qore features
set(CMAKE_REQUIRED_INCLUDES ${QORE_INCLUDE_DIR})
set(CMAKE_REQUIRED_LIBRARIES ${QORE_LIBRARY})
check_cxx_source_compiles("
#include <qore/Qore.h>
int main(void){
#if (QORE_MODULE_API_MAJOR == 0 && QORE_MODULE_API_MINOR < 7)
#error
#endif
return 0;
}" QORE_HAS_DATASOURCE_PORT)

check_cxx_source_compiles("
#include <qore/Qore.h>
int main(void){
Datasource *ds = nullptr;
ds->activeTransaction();
return 0;
}" HAVE_QORE_DATASOURCE_ACTIVE_TRANSACTION)

check_cxx_source_compiles("
#include <qore/Qore.h>
#include <qore/DBI.h>
static QoreValue execraw(Datasource*, const QoreString*, ExceptionSink*) {
    return QoreValue();
}
int main(void){
qore_dbi_method_list methods;
methods.add(QDBI_METHOD_EXECRAW, execraw);
return 0;
}" _QORE_HAS_DBI_EXECRAW)

check_cxx_source_compiles("
#include <qore/Qore.h>
#include <qore/DBI.h>
#include <qore/QoreColumnarResult.h>
int main(void){
#ifndef QORE_HAVE_COLUMNAR_RESULT_V2
#error missing QORE_HAVE_COLUMNAR_RESULT_V2
#endif
#ifndef QDBI_METHOD_SELECT_COLUMNAR
#error missing QDBI_METHOD_SELECT_COLUMNAR
#endif
#ifndef QDBI_METHOD_STMT_FETCH_COLUMNAR
#error missing QDBI_METHOD_STMT_FETCH_COLUMNAR
#endif
QoreColumnarResult* result = nullptr;
return result ? 1 : 0;
}" HAVE_QORE_COLUMNAR_RESULT)

check_cxx_source_compiles("
#include <qore/Qore.h>
#include <qore/DBI.h>
static int begin(Datasource*, const QoreString*, const QoreListNode*, const QoreHashNode*, ExceptionSink*) {
    return 0;
}
static int rows(Datasource*, const QoreHashNode*, ExceptionSink*) { return 0; }
static int end(Datasource*, bool, ExceptionSink*) { return 0; }
int main(void) {
    qore_dbi_method_list methods;
    methods.add(QDBI_METHOD_BULK_LOAD_BEGIN, begin);
    methods.add(QDBI_METHOD_BULK_LOAD_ROWS, rows);
    methods.add(QDBI_METHOD_BULK_LOAD_END, end);
    return DBI_CAP_HAS_BULK_LOAD ? 0 : 1;
}" HAVE_QORE_BULK_LOAD)

unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

check_cxx_compiler_flag(-fvisibility=hidden HAVE_GCC_VISIBILITY)

# Check for FreeTDS locale support
if(WITH_FREETDS)
    set(CMAKE_REQUIRED_INCLUDES ${FreeTDS_INCLUDE_DIR})
    set(CMAKE_REQUIRED_LIBRARIES ${FreeTDS_LIBS})
    check_cxx_source_compiles("
    #include <cspublic.h>
    int main(void){
    #ifndef CS_SYB_LANG_CHARSET
    #error
    #endif
    return 0;
    }" FREETDS_LOCALE)
    check_cxx_source_compiles("
    #include <bkpublic.h>
    int main(void) {
        CS_CONNECTION* connection = nullptr;
        CS_BLKDESC* descriptor = nullptr;
        CS_DATAFMT format = {};
        CS_INT length = 0;
        CS_SMALLINT indicator = 0;
        CS_INT rows = 0;
        blk_alloc(connection, BLK_VERSION_100, &descriptor);
        blk_init(descriptor, CS_BLK_IN, nullptr, CS_NULLTERM);
        blk_describe(descriptor, 1, &format);
        blk_bind(descriptor, 1, &format, nullptr, &length, &indicator);
        blk_rowxfer(descriptor);
        blk_done(descriptor, CS_BLK_ALL, &rows);
        blk_drop(descriptor);
        return 0;
    }" HAVE_FREETDS_BULK)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
endif()

# Platform detection
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LINUX TRUE)
    add_definitions(-D_GNU_SOURCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(DARWIN TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    set(SOLARIS TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "HP-UX")
    set(HPUX TRUE)
endif()

# Determine 32/64-bit
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MODULE_TARGET_BITS 64)
    set(SYB_LP64 TRUE)
else()
    set(MODULE_TARGET_BITS 32)
endif()

# Debug/Release mode
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DEBUG TRUE)
else()
    set(NDEBUG TRUE)
endif()

configure_file(${CMAKE_SOURCE_DIR}/cmake/config.h.cmake config.h)

# Source files - use single-compilation-unit mode to avoid circular header dependencies
set(CPP_SRC
    src/single-compilation-unit.cpp
)

set(QORE_DOX_TMPL_SRC
    docs/mainpage.dox.tmpl
    docs/guide-freetds_native_bulk_load.dox.tmpl
    docs/guide-sybase_timezone.dox.tmpl
    docs/guide-sybasebind.dox.tmpl
    docs/guide-sybaseoptions.dox.tmpl
    docs/guide-sybasereleasenotes.dox.tmpl
    docs/guide-sybasestoredprocs.dox.tmpl
)

# Resolve the documented native bulk-load API from the installed Qore SDK.
list(APPEND QORE_DOXYGEN_MODULES BulkSqlUtil)

# Build FreeTDS module
if(WITH_FREETDS)
    set(freetds_module_name "freetds")
    add_library(${freetds_module_name} MODULE ${CPP_SRC})
    target_compile_definitions(${freetds_module_name} PRIVATE FREETDS=1 HAVE_CONFIG_H=1)
    target_include_directories(${freetds_module_name} PRIVATE
        ${CMAKE_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${FreeTDS_INCLUDE_DIR}
    )
    if(DEFINED ENV{DOXYGEN_EXECUTABLE})
        set(DOXYGEN_EXECUTABLE $ENV{DOXYGEN_EXECUTABLE})
    endif()
    qore_external_binary_module(${freetds_module_name} ${PROJECT_VERSION} ${FreeTDS_LIBS} Threads::Threads)
endif()

# Build Sybase module
if(WITH_SYBASE)
    set(sybase_module_name "sybase")
    add_library(${sybase_module_name} MODULE ${CPP_SRC})
    target_compile_definitions(${sybase_module_name} PRIVATE SYBASE=1 HAVE_CONFIG_H=1)
    target_include_directories(${sybase_module_name} PRIVATE
        ${CMAKE_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${Sybase_INCLUDE_DIR}
    )
    qore_external_binary_module(${sybase_module_name} ${PROJECT_VERSION} ${Sybase_LIBS} Threads::Threads)
endif()

qore_dist(${PROJECT_VERSION})

qore_config_info()

# NOTE: find_package(Doxygen) called in qore_external_binary_module()
if(DOXYGEN_FOUND)
    qore_wrap_dox(QORE_DOX_SRC ${QORE_DOX_TMPL_SRC})
    add_custom_target(QORE_MOD_DOX_FILES DEPENDS ${QORE_DOX_SRC})
    add_dependencies(docs-module QORE_MOD_DOX_FILES)
endif()
