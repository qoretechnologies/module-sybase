/*
    sybase.cpp

    Sybase DB layer for QORE
    uses Sybase OpenClient C library

    Qore Programming language

    Copyright (C) 2003 - 2026 Qore Technologies, s.r.o.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <ctpublic.h>
#include <assert.h>
#include <ctype.h>
#include <memory>
#include <string>
#include <vector>

#include "sybase.h"

#ifdef HAVE_QORE_COLUMNAR_RESULT
#include <qore/QoreColumnarResult.h>
#endif

#include "connection.h"
#include "encoding_helpers.h"

static void sybase_module_init(QoreModuleInitContext& ctx, ExceptionSink& xsink);
static void sybase_module_ns_init(QoreNamespace* rns, QoreNamespace* qns, ExceptionSink& xsink);
static void sybase_module_delete();

static void sybase_module_desc_common(QoreModuleInfo& mod_info) {
    mod_info.version = PACKAGE_VERSION;
    mod_info.author = "Qore Technologies";
    mod_info.url = "http://qore.org";
    mod_info.api_major = QORE_MODULE_API_MAJOR;
    mod_info.api_minor = QORE_MODULE_API_MINOR;
    mod_info.init = sybase_module_init;
    mod_info.ns_init = sybase_module_ns_init;
    mod_info.del = sybase_module_delete;
    mod_info.license = QL_MIT;
    mod_info.license_str = "MIT";
}

extern "C" DLLEXPORT void sybase_qore_module_desc(QoreModuleInfo& mod_info) {
    mod_info.name = "sybase";
    mod_info.desc = "Sybase database driver";
    sybase_module_desc_common(mod_info);
}

#ifndef SYBASE
extern "C" DLLEXPORT void freetds_qore_module_desc(QoreModuleInfo& mod_info) {
    mod_info.name = "freetds";
    mod_info.desc = "FreeTDS-based database driver for MS-SQL Server and Sybase";
    sybase_module_desc_common(mod_info);
}
#endif
static DBIDriver* DBID_SYBASE;

// capabilities of this driver
int DBI_SYBASE_CAPS =
   DBI_CAP_TRANSACTION_MANAGEMENT
   | DBI_CAP_CHARSET_SUPPORT
   | DBI_CAP_LOB_SUPPORT
   | DBI_CAP_STORED_PROCEDURES
   | DBI_CAP_BIND_BY_VALUE
   | DBI_CAP_BIND_BY_PLACEHOLDER
   | DBI_CAP_HAS_NUMBER_SUPPORT
   | DBI_CAP_AUTORECONNECT
   | DBI_CAP_HAS_EXECRAW
   | DBI_CAP_TIME_ZONE_SUPPORT
   | DBI_CAP_SERVER_TIME_ZONE
   ;

#define BEGIN_CALLBACK \
       do { \
           try { do {} while(0)

#define END_CALLBACK(RV) \
           } catch (const ss::Error &e) { \
               e.raise(xsink); \
               return RV; \
           }\
      } while(0)

static int sybase_open(Datasource *ds, ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    // username is a required parameter
    if (!ds->getUsername()) {
        xsink->raiseException("DATASOURCE-MISSING-USERNAME", "Datasource has an empty username parameter");
        return -1;
    }

    // DB name is a required parameter
    if (!ds->getDBName()) {
        xsink->raiseException("DATASOURCE-MISSING-DBNAME", "Datasource has an empty dbname parameter");
        return -1;
    }

    // set the encoding for the connection
    if (ds->getDBEncoding()) {
        const QoreEncoding *enc = name_to_QoreEncoding(ds->getDBEncoding());
        ds->setQoreEncoding(enc);
    } else {
        const char *enc = QoreEncoding_to_SybaseName(QCS_DEFAULT);
        // if the encoding cannot be mapped, throw a Qore-language exception and return
        if (!enc) {
            xsink->raiseException("TDS-UNKNOWN-CHARACTER-SET", "cannot find the Sybase character encoding equivalent "
                "for '%s'", QCS_DEFAULT->getCode());
            return -1;
        }
        ds->setDBEncoding(enc);
        ds->setQoreEncoding(QCS_DEFAULT);
    }

    // create the connection object
    std::unique_ptr<connection> sc(new connection(ds, xsink));
    if (*xsink)
        return -1;

    int port = ds->getPort();
    const char* hostname = ds->getHostName();
    std::string hostname_str;

    // Parse hostname:port format if port is 0 but hostname contains a colon
    // This allows direct connections like freetds:user/pass@db%hostname:port
    if (!port && hostname) {
        const char* colon = strchr(hostname, ':');
        if (colon && colon[1]) {
            hostname_str.assign(hostname, colon - hostname);
            hostname = hostname_str.c_str();
            port = atoi(colon + 1);
            if (port <= 0 || port > 65535) {
                xsink->raiseException("TDS-CONNECT-ERROR", "invalid port number in hostname '%s'", ds->getHostName());
                return -1;
            }
        }
    }

    if (port && !hostname) {
        xsink->raiseException("TDS-CONNECT-ERROR", "port is set to %d, but no hostname is set; both hostname "
            "and port must be set to override the interfaces file", port);
        return -1;
    }

    if (!port && hostname) {
        xsink->raiseException("TDS-CONNECT-ERROR", "hostname is set to '%s', but no port is set; both hostname and "
            "port must be set to override the interfaces file", hostname);
        return -1;
    }

    // make the actual connection to the database
    sc->init(ds->getUsername(), ds->getPassword() ? ds->getPassword() : "", ds->getDBName(), ds->getDBEncoding(),
        ds->getQoreEncoding(), hostname, port, xsink);
    // return with an error if it didn't work
    if (*xsink)
        return -1;

    // set the private data
    ds->setPrivateData(sc.release());

    // return 0 for OK
    return 0;
    END_CALLBACK(-1);
}

static int sybase_close(Datasource *ds) {
    try {
        connection* sc = (connection*)ds->getPrivateData();
        ds->setPrivateData(0);
        delete sc;
    } catch (const ss::Error &e) {}
    return 0;
}

static QoreValue sybase_select(Datasource *ds, const QoreString *qstr, const QoreListNode *args,
        ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    connection* conn = (connection*)ds->getPrivateData();
    return conn->select(qstr, args, xsink);
    END_CALLBACK(0);
}

#ifdef QDBI_METHOD_SELECT_TYPED
static QoreValue sybase_select_typed(Datasource *ds, const QoreString *qstr, const QoreListNode *args,
        ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    connection* conn = static_cast<connection*>(ds->getPrivateData());
    return conn->selectTyped(qstr, args, xsink);
    END_CALLBACK(0);
}
#endif

#ifdef HAVE_QORE_COLUMNAR_RESULT
static QoreColumnarResult* sybase_select_columnar(Datasource *ds, const QoreString *qstr, const QoreListNode *args,
        ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    connection* conn = (connection*)ds->getPrivateData();
    return conn->selectColumnar(qstr, args, xsink);
    END_CALLBACK(0);
}
#endif

static QoreHashNode* sybase_select_row(Datasource *ds, const QoreString *qstr, const QoreListNode *args,
        ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    connection *conn = (connection*)ds->getPrivateData();
    ValueHolder holder(conn->exec_row(qstr, args, xsink), xsink);
    qore_type_t nt = holder->getType();
    if (nt != NT_HASH && nt != NT_NOTHING) {
        if (!*xsink)
            xsink->raiseException("DBI-SELECT-ROW-ERROR", "selectRow() returned type '%s'; expecting a hash for a "
                "single row", holder->getTypeName());
        holder.release().discard(xsink);
    }
    return holder ? holder.release().get<QoreHashNode>() : nullptr;
    END_CALLBACK(0);
}

static QoreValue sybase_select_rows(Datasource *ds, const QoreString *qstr, const QoreListNode *args,
        ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    connection *conn = (connection*)ds->getPrivateData();
    QoreValue rv = conn->exec_rows(qstr, args, xsink);
    if (rv.getType() == NT_HASH) {
        QoreListNode* l = new QoreListNode(autoTypeInfo);
        l->push(rv, xsink);
        rv = l;
    }
    return rv;
    //return conn->exec_rows(qstr, args, xsink);
    END_CALLBACK(0);
}

static QoreValue sybase_exec(Datasource *ds, const QoreString *qstr, const QoreListNode *args,
        ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    connection *conn = (connection*)ds->getPrivateData();
    QoreValue rv = conn->exec(qstr, args, xsink);
    if (!rv) {
        rv = 0;
    }
    return rv;
    END_CALLBACK(0);
}

static QoreValue sybase_execRaw(Datasource *ds, const QoreString *qstr, ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    connection *conn = (connection*)ds->getPrivateData();
    QoreValue rv = conn->execRaw(qstr, xsink);
    if (!rv) {
        rv = 0;
    }
    return rv;
    END_CALLBACK(0);
}

static int sybase_commit(Datasource *ds, ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    connection* conn = (connection*)ds->getPrivateData();
    return conn->commit(xsink);
    END_CALLBACK(0);
}

static int sybase_rollback(Datasource *ds, ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    connection* conn = (connection*)ds->getPrivateData();
    return conn->rollback(xsink);
    END_CALLBACK(0);
}

#if defined(FREETDS) && defined(HAVE_QORE_BULK_LOAD) && defined(HAVE_FREETDS_BULK)
static int sybase_bulk_load_begin(Datasource* ds, const QoreString* table, const QoreListNode* columns,
        const QoreHashNode* options, ExceptionSink* xsink) {
    BEGIN_CALLBACK;
    connection* conn = static_cast<connection*>(ds->getPrivateData());
    return conn->bulkLoadBegin(table, columns, options, xsink);
    END_CALLBACK(-1);
}

static int sybase_bulk_load_rows(Datasource* ds, const QoreHashNode* rows, ExceptionSink* xsink) {
    BEGIN_CALLBACK;
    connection* conn = static_cast<connection*>(ds->getPrivateData());
    return conn->bulkLoadRows(rows, xsink);
    END_CALLBACK(-1);
}

static int sybase_bulk_load_end(Datasource* ds, bool success, ExceptionSink* xsink) {
    BEGIN_CALLBACK;
    connection* conn = static_cast<connection*>(ds->getPrivateData());
    return conn->bulkLoadEnd(success, xsink);
    END_CALLBACK(-1);
}
#endif

static QoreValue sybase_get_client_version(const Datasource *ds, ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    context m_context(xsink);
    if (!m_context)
        return QoreValue();

    return m_context.get_client_version(xsink);
    END_CALLBACK(0);
}

static QoreValue sybase_get_server_version(Datasource *ds, ExceptionSink *xsink) {
    BEGIN_CALLBACK;
    connection* conn = (connection*)ds->getPrivateData();
    return conn->get_server_version(xsink);
    END_CALLBACK(0);
}

static int sybase_opt_set(Datasource* ds, const char* opt, const QoreValue val, ExceptionSink* xsink) {
    BEGIN_CALLBACK;
    connection *conn = (connection*)ds->getPrivateData();
    return conn->setOption(opt, val, xsink);
    END_CALLBACK(0);
}

static QoreValue sybase_opt_get(const Datasource* ds, const char* opt) {
    try {
        connection *conn = (connection*)ds->getPrivateData();
        return conn->getOption(opt);
    } catch (const ss::Error &e) {
        return QoreValue();
    }
}

namespace ss {
    void init(qore_dbi_method_list &methods);
}

static void sybase_module_init(QoreModuleInitContext& ctx, ExceptionSink& xsink) {
    QORE_TRACE("sybase_module_init()");

    // init_namespace();

    // register driver with DBI subsystem
    qore_dbi_method_list methods;
    methods.add(QDBI_METHOD_OPEN, sybase_open);
    methods.add(QDBI_METHOD_CLOSE, sybase_close);
    methods.add(QDBI_METHOD_SELECT, sybase_select);
#ifdef QDBI_METHOD_SELECT_TYPED
    methods.add(QDBI_METHOD_SELECT_TYPED, sybase_select_typed);
#endif
#ifdef HAVE_QORE_COLUMNAR_RESULT
    methods.add(QDBI_METHOD_SELECT_COLUMNAR, sybase_select_columnar);
#endif
    methods.add(QDBI_METHOD_SELECT_ROW, sybase_select_row);
    methods.add(QDBI_METHOD_SELECT_ROWS, sybase_select_rows);
    methods.add(QDBI_METHOD_EXEC, sybase_exec);
    methods.add(QDBI_METHOD_EXECRAW, sybase_execRaw);
    methods.add(QDBI_METHOD_COMMIT, sybase_commit);
    methods.add(QDBI_METHOD_ROLLBACK, sybase_rollback);
#if defined(FREETDS) && defined(HAVE_QORE_BULK_LOAD) && defined(HAVE_FREETDS_BULK)
    methods.add(QDBI_METHOD_BULK_LOAD_BEGIN, sybase_bulk_load_begin);
    methods.add(QDBI_METHOD_BULK_LOAD_ROWS, sybase_bulk_load_rows);
    methods.add(QDBI_METHOD_BULK_LOAD_END, sybase_bulk_load_end);
#endif
    methods.add(QDBI_METHOD_GET_CLIENT_VERSION, sybase_get_client_version);
    methods.add(QDBI_METHOD_GET_SERVER_VERSION, sybase_get_server_version);

    methods.add(QDBI_METHOD_OPT_SET, sybase_opt_set);
    methods.add(QDBI_METHOD_OPT_GET, sybase_opt_get);

    methods.registerOption(DBI_OPT_NUMBER_OPT, "when set, numeric/decimal values are returned as integers if "
        "possible, otherwise as arbitrary-precision number values; the argument is ignored; setting this option "
        "turns it on and turns off 'string-numbers' and 'numeric-numbers'");
    methods.registerOption(DBI_OPT_NUMBER_STRING, "when set, numeric/decimal values are returned as strings for "
        "backwards-compatibility; the argument is ignored; setting this option turns it on and turns off "
        "'optimal-numbers' and 'numeric-numbers'");
    methods.registerOption(DBI_OPT_NUMBER_NUMERIC, "when set, numeric/decimal values are returned as "
        "arbitrary-precision number values; the argument is ignored; setting this option turns it on and turns off "
        "'string-numbers' and 'optimal-numbers'");
    methods.registerOption(DBI_OPT_TIMEZONE, "set the server-side timezone, the value must be a string in the format "
        "accepted by Timezone::constructor() on the client (ie either a region name or a UTC offset like "
        "\"+01:00\"); if not set the server's time zone will be assumed to be the same as the client's",
        stringTypeInfo);
    methods.registerOption(SYBASE_OPT_OPTIMIZED_DATE_BINDS, "when set, date/time values are bound with full "
        "resolution including microseconds, however this will cause any operations with date/time values with "
        "microseconds bound for DATETIME columns to fail, if this is not set, then date/time values are bound with "
        "an approach that works for all columns but gives a maximum of 1/300 second resolution");
#ifndef SYBASE
    methods.registerOption(SYBASE_OPT_TDS_VERSION, "sets the TDS protocol version used for the connection; this "
        "allows ad-hoc connections to be made without requiring a freetds.conf entry for the server; the value "
        "must be a string: one of \"auto\", \"4.0\", \"4.2\", \"4.6\", \"4.9.5\", \"5.0\", \"7.0\", \"7.1\", "
        "\"7.2\", \"7.3\", or \"7.4\"; use \"5.0\" for Sybase ASE or \"7.x\" for MS SQL Server; if not set, the "
        "FreeTDS default or freetds.conf configuration is used; takes effect on the next connection",
        stringTypeInfo);
#endif

    ss::init(methods);

#ifdef SYBASE
    DBID_SYBASE = DBI.registerDriver("sybase", methods, DBI_SYBASE_CAPS);
#else
    DBID_SYBASE = DBI.registerDriver("freetds", methods, DBI_SYBASE_CAPS);
#endif
}

static void sybase_module_ns_init(QoreNamespace* rns, QoreNamespace* qns, ExceptionSink& xsink) {
}

static void sybase_module_delete() {
    QORE_TRACE("sybase_module_delete()");
}
