/*
    connection.h

    Sybase DB layer for QORE
    uses Sybase OpenClient C library

    Qore Programming language

    Copyright (C) 2007 - 2026 Qore Technologies, s.r.o.

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

#ifndef SYBASE_CONNECTION_H_
#define SYBASE_CONNECTION_H_

#include "config.h"

#include <cstypes.h>
#include <ctpublic.h>
#include <stdarg.h>

#include <atomic>
#include <memory>

#include "qore/common.h"
#include "qore/ExceptionSink.h"
#include "qore/QoreSandboxManager.h"

#include "command.h"
#include "dbmodulewrap.h"
#include "statement.h"

#if defined(SYBASE) || defined(FREETDS_LOCALE)
#define SYB_HAVE_LOCALE 1
#else
#undef SYB_HAVE_LOCALE
#endif

#ifndef CLIENT_VER_LEN
#define CLIENT_VER_LEN 240
#endif

#ifdef SYBASE
extern QoreThreadLock ct_lock;
extern QoreThreadLock cs_lock;
#endif

class AbstractQoreZoneInfo;

#if defined(FREETDS) && defined(HAVE_QORE_BULK_LOAD) && defined(HAVE_FREETDS_BULK)
class QoreSybaseBulkLoadState;
#endif

typedef ss::DBModuleWrap<ss::Statement>::ModuleWrap stmt_t;

class context {
public:
    DLLLOCAL context(ExceptionSink *xsink) {
        CS_RETCODE ret;

#ifdef SYBASE
        {
            AutoLocker al(cs_lock);
#endif
            ret = cs_ctx_alloc(CS_VERSION_100, &m_context);
#ifdef SYBASE
        }
#endif
         if (ret != CS_SUCCEED) {
            xsink->raiseException("TDS-CT-LIB-CANNOT-ALLOCATE-ERROR", "cs_ctx_alloc() failed with error %d", ret);
            return;
        }

#ifdef SYBASE
        {
            AutoLocker al(ct_lock);
#endif
            ret = ct_init(m_context, CS_VERSION_100);
#ifdef SYBASE
        }
#endif
        if (ret != CS_SUCCEED) {
            del();
            xsink->raiseException("TDS-CT-LIB-INIT-FAILED", "ct_init() failed with error %d", ret);
            return;
        }
        //printd(5, "context::context() this=%p m_context=%p\n", this, m_context);
    }

    DLLLOCAL ~context() {
        if (m_context)
            del();
    }

    DLLLOCAL operator bool() const {
        return (bool)m_context;
    }

    DLLLOCAL CS_CONTEXT* get_context() { return m_context; }

    DLLLOCAL QoreStringNode* get_client_version(ExceptionSink *xsink) {
        char *buf = (char *)malloc(sizeof(char) * CLIENT_VER_LEN);
        CS_INT olen = 0;
        CS_RETCODE ret = ct_config(m_context, CS_GET, CS_VER_STRING, buf, CLIENT_VER_LEN, &olen);
        //printd(5, "olen=%d, ret=%d\n", olen, ret);
        if (ret != CS_SUCCEED) {
            free(buf);
            xsink->raiseException("TDS-GET-CLIENT-VERSION-ERROR", "ct_config(CS_VER_STRING) failed with error %d", (int)ret);
            return nullptr;
        }
        //printd(5, "client version=%s (olen=%d, strlen=%d)\n", buf, olen, strlen(buf));
        return new QoreStringNode(buf, olen - 1, CLIENT_VER_LEN, QCS_DEFAULT);
    }

private:
    CS_CONTEXT* m_context;

    DLLLOCAL void del() {
        //printd(5, "context::del() this=%p deleting %p\n", this, m_context);

        CS_RETCODE ret;
#ifdef SYBASE
        {
            AutoLocker al(ct_lock);
#endif
            ret = ct_exit(m_context, CS_UNUSED);
#ifdef SYBASE
        }
#endif
        if (ret != CS_SUCCEED) {
#ifdef SYBASE
            {
                AutoLocker al(ct_lock);
#endif
                ret = ct_exit(m_context, CS_FORCE_EXIT);
#ifdef SYBASE
            }
#endif
            assert(ret == CS_SUCCEED);
        }
#ifdef SYBASE
        AutoLocker al(cs_lock);
#endif
        ret = cs_ctx_drop(m_context);
        assert(ret == CS_SUCCEED);
    }
};

//! RAII helper for Sybase statement cancellation
/** Registers a cancel callback with the sandbox manager before blocking operations.
    When requestInterrupt() is called, the callback will use ct_cancel() to cancel the operation.
*/
class QoreSybaseCancelHelper {
public:
    DLLLOCAL QoreSybaseCancelHelper(CS_CONNECTION* conn);
    DLLLOCAL ~QoreSybaseCancelHelper();

    // Non-copyable
    QoreSybaseCancelHelper(const QoreSybaseCancelHelper&) = delete;
    QoreSybaseCancelHelper& operator=(const QoreSybaseCancelHelper&) = delete;

private:
    struct State;
    std::shared_ptr<State> state;
    QoreSandboxManagerHelper smh;
};

// Instantiated class is kept as private data of the Datasource
// for the time the Datasource exists. All other Sybase
// resources are shortlived (including CS_COMMAND* and its wrapper).
class connection {
public:
    static const int OPT_NUM_OPTIMAL = 0;
    static const int OPT_NUM_STRING = 1;
    static const int OPT_NUM_NUMERIC = 2;

    DLLLOCAL connection(Datasource *n_ds, ExceptionSink *xsink);
    DLLLOCAL ~connection();

    DLLLOCAL command* setupCommand(const QoreString* cmd_text, const QoreListNode* args, bool raw, ExceptionSink* xsink);

    // to be called after the object is constructed
    // returns 0=OK, -1=error (exception raised)
    DLLLOCAL int init(const char *username, const char *password, const char *dbname,
            const char *db_encoding, const QoreEncoding *n_enc, const char *hostname,
            int port, ExceptionSink* xsink);

    // returns 0=OK, -1=error (exception raised)
    DLLLOCAL int purge_messages(ExceptionSink *xsink);
    // discard all messages
    DLLLOCAL void discard_messages();
    // returns -1
    DLLLOCAL void do_exception(ExceptionSink *xsink, const char *err, const char *fmt, ...);
    // returns 0=OK, -1=error (exception raised)
    DLLLOCAL int direct_execute(const char *sql_text, ExceptionSink *xsink);

    // returns 0=OK, -1=error (exception raised)
    DLLLOCAL int commit(ExceptionSink *xsink);
    // returns 0=OK, -1=error (exception raised)
    DLLLOCAL int rollback(ExceptionSink *xsink);

#if defined(FREETDS) && defined(HAVE_QORE_BULK_LOAD) && defined(HAVE_FREETDS_BULK)
    DLLLOCAL int bulkLoadBegin(const QoreString* table, const QoreListNode* columns,
            const QoreHashNode* options, ExceptionSink* xsink);
    DLLLOCAL int bulkLoadRows(const QoreHashNode* rows, ExceptionSink* xsink);
    DLLLOCAL int bulkLoadEnd(bool success, ExceptionSink* xsink);
#endif

    //! executes the SQL and reads every result
    /** @param only_result_set if not null, the call returns one result set (see command::readOutput())
    */
    DLLLOCAL QoreValue execReadOutput(QoreString *cmd_text, const QoreListNode *qore_args, bool need_list,
            bool doBinding, bool cols, ExceptionSink* xsink, bool single_row = false,
            const char* only_result_set = nullptr, bool typed = false);
    DLLLOCAL command::ResType readNextResult(command& cmd, bool& connection_reset, ExceptionSink* xsink);

    DLLLOCAL QoreValue select(const QoreString *cmd, const QoreListNode *parameters, ExceptionSink *xsink);
#ifdef QDBI_METHOD_SELECT_TYPED
    //! returns every result as select() does, with each result set as a typed result
    DLLLOCAL QoreValue selectTyped(const QoreString *cmd, const QoreListNode *parameters, ExceptionSink *xsink);
#endif
#ifdef HAVE_QORE_COLUMNAR_RESULT
    DLLLOCAL QoreColumnarResult* selectColumnar(const QoreString *cmd, const QoreListNode *parameters,
            ExceptionSink *xsink);
#endif

    DLLLOCAL QoreValue exec(const QoreString *cmd, const QoreListNode *parameters, ExceptionSink *xsink);
    DLLLOCAL QoreValue execRaw(const QoreString *cmd, ExceptionSink *xsink);
    DLLLOCAL QoreValue exec_rows(const QoreString *cmd, const QoreListNode *parameters, ExceptionSink *xsink);
    DLLLOCAL QoreValue exec_row(const QoreString *cmd, const QoreListNode *parameters, ExceptionSink *xsink);

    // returns true if the server is still reachable on the connection, false if not
    DLLLOCAL bool ping() const;

    // invalidate / close any open statement
    DLLLOCAL void invalidateStatement() {
        if (stmt) {
            stmt->invalidate();
            stmt = nullptr;
            // the messages of the statement's results that were not read are discarded with them, so that they are
            // not raised by the next call
            if (m_connection) {
                discard_messages();
            }
        }
    }

    DLLLOCAL bool wasConnectionAborted() const {
        return ds->wasConnectionAborted();
    }

    DLLLOCAL void registerStatement(stmt_t* n_stmt) {
        // invalidate any existing statement
        if (stmt)
            stmt->invalidate();
        stmt = n_stmt;
    }

#ifdef DEBUG
    DLLLOCAL void deregisterStatement(stmt_t* n_stmt) {
        assert(n_stmt == stmt);
#else
       DLLLOCAL void deregisterStatement() {
#endif
        assert(stmt);
        stmt = 0;
    }

    DLLLOCAL CS_CONNECTION* getConnection() const { return m_connection; }
    DLLLOCAL CS_CONTEXT* getContext() { return m_context.get_context(); }
    DLLLOCAL const QoreEncoding *getEncoding() const { return enc; }
    DLLLOCAL Datasource* getDatasource() const { return ds; }

    DLLLOCAL QoreStringNode *get_client_version(ExceptionSink *xsink);
    DLLLOCAL QoreValue get_server_version(ExceptionSink *xsink);

    DLLLOCAL int setOption(const char* opt, QoreValue val, ExceptionSink* xsink);
    DLLLOCAL QoreValue getOption(const char* opt);

    DLLLOCAL int getNumeric() const { return numeric_support; }

    // issue #4710: returns true if the server was positively identified as MS SQL Server
    DLLLOCAL bool isMsSql() const { return mssql; }

    DLLLOCAL const AbstractQoreZoneInfo* getTZ() const;

    DLLLOCAL bool optimizedDateBinds() const {
        return optimized_date_binds;
    }

#ifndef SYBASE
    // issue #4321: parse a "tds-version" option value into a CS_TDS_* constant;
    // returns 0 on success (sets ver), -1 on error (exception raised)
    DLLLOCAL static int parseTdsVersion(QoreValue val, CS_INT& ver, ExceptionSink* xsink);
    // issue #4321: return the canonical string for a CS_TDS_* constant or nullptr if unknown
    DLLLOCAL static const char* tdsVersionString(CS_INT ver);
#endif

private:
    context m_context;
    CS_CONNECTION* m_connection = nullptr;
    bool connected = false;
    bool sybase = false;
    // issue #4710: set to true when the server is positively identified as MS SQL Server
    bool mssql = false;
    const QoreEncoding* enc = nullptr;
    Datasource* ds;
    int numeric_support = OPT_NUM_OPTIMAL;
    int64 login_timeout = 60;
    const AbstractQoreZoneInfo* server_tz = nullptr;
    bool optimized_date_binds = false;
#ifndef SYBASE
    // issue #4321: TDS protocol version to set programmatically before connecting;
    // tds_version_set is false when not configured (use freetds.conf / FreeTDS default)
    CS_INT tds_version = CS_TDS_AUTO;
    bool tds_version_set = false;
#endif

    stmt_t* stmt = nullptr;

#if defined(FREETDS) && defined(HAVE_QORE_BULK_LOAD) && defined(HAVE_FREETDS_BULK)
    std::unique_ptr<QoreSybaseBulkLoadState> bulk_load;
#endif

    // returns -1 if an exception was thrown, 0 if all errors were ignored
    DLLLOCAL void do_check_exception(ExceptionSink *xsink, bool check, const char *err, const char *fmt, ...);
    // returns -1 if an exception was thrown, 0 if all errors were ignored
    DLLLOCAL void do_check_exception(ExceptionSink *xsink, bool check, const char *err, QoreStringNode* estr);

    // returns 0 if reconnected without any errors, -1 if there were errors (transaction in progress, reconnect failed, etc)
    DLLLOCAL int closeAndReconnect(ExceptionSink* xsink, command& cmd, bool try_reconnect = true);
};

constexpr const char* SYBASE_OPT_OPTIMIZED_DATE_BINDS = "optimized-date-binds";
constexpr const char* SYBASE_OPT_LOGIN_TIMEOUT = "login-timeout";
#ifndef SYBASE
// issue #4321: option to set the TDS protocol version programmatically for ad-hoc
// connections without requiring a freetds.conf entry
constexpr const char* SYBASE_OPT_TDS_VERSION = "tds-version";
#endif

#endif

// EOF
