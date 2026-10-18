/*
    connection.cpp

    Sybase DB layer for QORE
    uses Sybase OpenClient C library or FreeTDS's ct-lib

    Qore Programming Language

    Copyright (C) 2007 - 2026 Qore Technologies s.r.o.

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

#include <assert.h>
#include <memory>

#include <ctpublic.h>

#include "sybase.h"

#ifdef HAVE_QORE_COLUMNAR_RESULT
#include <qore/QoreColumnarResult.h>
#endif

#include "connection.h"
#if defined(FREETDS) && defined(HAVE_QORE_BULK_LOAD) && defined(HAVE_FREETDS_BULK)
#include "bulk_load.h"
#endif
#include "encoding_helpers.h"
#include "sybase_query.h"
#include "command.h"

static QoreString ver_str("begin tran select @@version commit tran");

#ifdef SYBASE
// to serialize calls to ct_init() and ct_exit()
QoreThreadLock ct_lock;
QoreThreadLock cs_lock;
#endif

//------------------------------------------------------------------------------
// QoreSybaseCancelHelper implementation
//------------------------------------------------------------------------------

QoreSybaseCancelHelper::QoreSybaseCancelHelper(CS_CONNECTION* conn)
    : conn(conn) {
    if (smh && conn) {
        // Register cancel callback
        smh->registerCancelCallback(this, [this]() -> bool {
            // Load pointer atomically - it may be set to nullptr by destructor
            CS_CONNECTION* c = this->conn.load(std::memory_order_acquire);
            if (c) {
                // ct_cancel with CS_CANCEL_ALL cancels the current operation on the connection
                CS_RETCODE rc = ct_cancel(c, nullptr, CS_CANCEL_ALL);
                return rc == CS_SUCCEED;
            }
            return false;
        });
    }
}

QoreSybaseCancelHelper::~QoreSybaseCancelHelper() {
    // Set pointer to nullptr atomically before unregistering to prevent
    // use-after-free if a callback is currently being invoked
    conn.store(nullptr, std::memory_order_release);
    if (smh) {
        smh->unregisterCancelCallback(this);
    }
}

connection::connection(Datasource *n_ds, ExceptionSink *xsink) :
        m_context(xsink),
        ds(n_ds) {
}

connection::~connection() {
    invalidateStatement();
#if defined(FREETDS) && defined(HAVE_QORE_BULK_LOAD) && defined(HAVE_FREETDS_BULK)
    bulk_load.reset();
#endif
    CS_RETCODE ret = CS_SUCCEED;

    if (m_connection) {
        if (connected) {
            ret = ct_close(m_connection, CS_UNUSED);
            if (ret != CS_SUCCEED) {
                ret = ct_close(m_connection, CS_FORCE_CLOSE);
                assert(ret == CS_SUCCEED);
            }
        }
        ret = ct_con_drop(m_connection);
        assert(ret == CS_SUCCEED);
    }
}

// FIXME: check for auto-reconnect here if ct_command fails
int connection::direct_execute(const char* sql_text, ExceptionSink* xsink) {
    assert(sql_text && sql_text[0]);

    CS_COMMAND* cmd = 0;

    CS_RETCODE err = ct_cmd_alloc(m_connection, &cmd);
    if (err != CS_SUCCEED)
        do_exception(xsink, "TDS-EXEC-ERROR", "ct_cmd_alloc() failed");

    ON_BLOCK_EXIT(ct_cmd_drop, cmd);
    ScopeGuard canceller = MakeGuard(ct_cancel, (CS_CONNECTION*)0, cmd, CS_CANCEL_ALL);

    err = ct_command(cmd, CS_LANG_CMD, (CS_CHAR*)sql_text, strlen(sql_text), CS_END);
    if (err != CS_SUCCEED)
        do_exception(xsink, "TDS-EXEC-ERROR", "ct_command() failed");

    // Check for interrupt before sending command
    if (qore_check_cancel(xsink))
        return -1;

    {
        // Register cancel callback for interruptible execution
        QoreSybaseCancelHelper cancel_helper(m_connection);
        err = ct_send(cmd);
    }
    if (err != CS_SUCCEED)
        do_exception(xsink, "TDS-EXEC-ERROR", "ct_send() failed");

    // no results expected
    CS_INT result_type;
    {
        // Register cancel callback for interruptible results fetch
        QoreSybaseCancelHelper cancel_helper(m_connection);
        err = ct_results(cmd, &result_type);
    }
    if (err != CS_SUCCEED)
        do_exception(xsink, "TDS-EXEC-ERROR",
                    "connection::direct_execute(): ct_results()"
                    " returned error code %d", err);

    if (result_type != CS_CMD_SUCCEED)
        do_check_exception(xsink, true, "TDS-EXEC-ERROR",
                            "connection::direct_execute(): ct_results()"
                            " failed with result_type = %d", result_type);

    {
        // Move cancel helper outside the loop for efficiency
        QoreSybaseCancelHelper cancel_helper(m_connection);
        while (true) {
            err = ct_results(cmd, &result_type);
            if (err != CS_SUCCEED)
                break;
        }
    }
    canceller.Dismiss();

    return purge_messages(xsink);
}

static inline bool wasInTransaction(Datasource *ds) {
#if defined(_QORE_HAS_DATASOURCE_ACTIVETRANSACTION) || defined(HAVE_QORE_DATASOURCE_ACTIVE_TRANSACTION)
    return ds->activeTransaction();
#else
    return ds->isInTransaction();
#endif
}

bool connection::ping() const {
    // check if the connection is up
    CS_INT up;
    CS_INT outlen;
    CS_RETCODE rc = ct_con_props(m_connection, CS_GET, CS_CON_STATUS, &up, sizeof(up), &outlen);
    printd(5, "ping() rc: %d raw: %d up: %d\n", rc, up, up == CS_CONSTAT_CONNECTED);
    return (rc == CS_SUCCEED) && (up == CS_CONSTAT_CONNECTED);
}

command* connection::setupCommand(const QoreString* cmd_text, const QoreListNode* args, bool raw,
        ExceptionSink* xsink) {
    while (true) {
        std::unique_ptr<sybase_query> query(new sybase_query);
        if (!raw) {
            if (query->init(cmd_text, args, isMsSql(), xsink))
                return nullptr;
        } else {
            assert(!args);
            query->init(cmd_text);
        }

        std::unique_ptr<command> cmd(new command(*this, xsink));
        // the command could not be allocated, or the query could not be bound: the exception is raised, and no
        // command is returned
        if (*xsink) {
            return nullptr;
        }
        cmd->bind_query(query, args, xsink);
        if (*xsink) {
            return nullptr;
        }

        try {
            cmd->send(xsink);
        } catch (const ss::Error& e) {
            // if the connection is down and we can reconnect transparently, then we do so
            if (!ping() && !closeAndReconnect(xsink, *cmd.get(), true))
                continue;
            throw;
        }
        return cmd.release();
    }
}

QoreValue connection::execReadOutput(QoreString* cmd_text, const QoreListNode* qore_args, bool need_list,
        bool doBinding, bool cols, ExceptionSink* xsink, bool single_row) {
    // cancel any active statement
    invalidateStatement();

    std::unique_ptr<command> cmd(setupCommand(cmd_text, qore_args, !doBinding, xsink));
    // no command is returned when the query cannot be parsed or bound; the exception has been raised
    // (dereferencing the null command here let an optimizing compiler drop the null check when the command was
    // destroyed, which crashed in command::clear())
    if (*xsink || !cmd) {
        assert(*xsink);
        return QoreValue();
    }

    bool connection_reset = false;

    ValueHolder result(xsink);

    while (true) {
        result = cmd->readOutput(*this, *cmd.get(), need_list, connection_reset, cols, xsink, single_row);
        if (*xsink)
            return QoreValue();

        if (connection_reset) {
            connection_reset = false;
            continue;
        }

        break;
    }

    return result.release();
}

int connection::closeAndReconnect(ExceptionSink* xsink, command& cmd, bool try_reconnect) {
    // cancel the current command
    cmd.cancelDisconnect();

    // cancel any current statement
    invalidateStatement();

    // see if we need to reconnect and try again
    ct_close(m_connection, CS_FORCE_CLOSE);
    connected = false;

    // discard all current messages
    discard_messages();

    if (wasInTransaction(ds))
        xsink->raiseException("TDS-TRANSACTION-ERROR", "connection to server lost while in a transaction; "
            "transaction has been lost");

    // otherwise try to reconnect
    ct_con_drop(m_connection);
    m_connection = nullptr;

    int port = ds->getPort();

    printd(5, "connection::closeAndReconnect() this: %p reconnecting to %s@%s (trans: %d, try_reconnect: %d)\n", this,
        ds->getUsername(), ds->getDBName(), wasInTransaction(ds), try_reconnect);

    // make the actual connection to the database
    if (init(ds->getUsername(), ds->getPassword() ? ds->getPassword() : "", ds->getDBName(), ds->getDBEncoding(),
        ds->getQoreEncoding(), ds->getHostName(), port, xsink)) {
        // make sure and mark Datasource as closed
        ds->connectionAborted();
        return -1;
    }

    printd(5, "connection::closeAndReconnect() this: %p auto reconnected to %s@%s m_connection: %p\n", this,
        ds->getUsername(), ds->getDBName(), m_connection);

    if (!try_reconnect || wasInTransaction(ds))
        return -1;

#ifdef DEBUG
    // otherwise show the exception on stdout in debug mode
    xsink->handleExceptions();
#endif

    // clear any exceptions that have been ignored
    xsink->clear();

    return 0;
}

command::ResType connection::readNextResult(command& cmd, bool& connection_reset, ExceptionSink* xsink) {
    assert(!connection_reset);

    while (true) {
        if (*xsink)
            return command::RES_ERROR;

        bool disconnect = false;

        command::ResType rc = cmd.read_next_result(disconnect, xsink);
        if (!disconnect)
            return rc;

        closeAndReconnect(xsink, cmd, false);
        return command::RES_ERROR;
    }

    // to avoid warnings; not actually executed
    return command::RES_NONE;
}

/*
AbstractQoreNode *connection::exec_intern(QoreString *cmd_text, const QoreListNode *qore_args, bool need_list, ExceptionSink* xsink, bool doBinding) {
   std::unique_ptr<command> cmd;
   if (doBinding)
      cmd.reset(create_command(cmd_text, qore_args, xsink));
   else
      cmd.reset(create_command(cmd_text, xsink));

   while (true) {
      if (*xsink)
         return 0;

      bool disconnect = false;

      ReferenceHolder<AbstractQoreNode>
         result(cmd->read_output(need_list, disconnect, xsink), xsink);

      if (*xsink)
         return 0;

      if (!disconnect)
         return result.release();

      // see if we need to reconnect and try again
      ct_close(m_connection, CS_FORCE_CLOSE);
      connected = false;

      // discard all current messages
      discard_messages();

      if (wasInTransaction(ds))
         xsink->raiseException("SYBASE-TRANSACTION-ERROR",
                               "connection to server lost while in a transaction; transaction has been lost");

      // otherwise try to reconnect
      ct_con_drop(m_connection);
      m_connection = 0;

#ifdef QORE_HAS_DATASOURCE_PORT
      int port = ds->getPort();
#else
      int port = 0;
#endif

      // make the actual connection to the database
      init(ds->getUsername(), ds->getPassword() ? ds->getPassword() : "", ds->getDBName(), ds->getDBEncoding(), ds->getQoreEncoding(), ds->getHostName(), port, xsink);
      // return with an error if it didn't work
      if (*xsink) {
         // make sure and mark Datasource as closed
         ds->connectionAborted();
         return 0;
      }

      // if the connection was aborted while in a transaction, return now
      if (wasInTransaction(ds))
         return 0;

#ifdef DEBUG
      // otherwise show the exception on stdout in debug mode
      xsink->handleExceptions();
#endif
      // clear any exceptions that have been ignored
      xsink->clear();

      printd(5, "connection::exec_intern() this: %p auto reconnected to %s@%s\n", this, ds->getUsername(), ds->getDBName());
   }

   // to avoid warnings
   return 0;
}
*/

QoreValue connection::select(const QoreString *cmd, const QoreListNode* args, ExceptionSink *xsink) {
    // copy the string here for intrusive editing, convert encoding too if necessary
    QoreString *query = cmd->convertEncoding(enc, xsink);
    if (!query)
        return QoreValue();

    std::unique_ptr<QoreString> tmp(query);
    ValueHolder rv(execReadOutput(query, args, false, true, true, xsink), xsink);
    purge_messages(xsink);
    return rv.release();
}

#ifdef HAVE_QORE_COLUMNAR_RESULT
QoreColumnarResult* connection::selectColumnar(const QoreString *cmd, const QoreListNode* args, ExceptionSink *xsink) {
    QoreString *query = cmd->convertEncoding(enc, xsink);
    if (!query) {
        return nullptr;
    }

    std::unique_ptr<QoreString> tmp(query);

    invalidateStatement();

    std::unique_ptr<command> sql_command(setupCommand(query, args, false, xsink));
    if (*xsink) {
        purge_messages(xsink);
        return nullptr;
    }

    bool connection_reset = false;
    while (true) {
        command::ResType rt = readNextResult(*sql_command, connection_reset, xsink);
        if (*xsink || connection_reset) {
            purge_messages(xsink);
            return nullptr;
        }

        switch (rt) {
            case command::RES_ROW: {
                ReferenceHolder<QoreColumnarResult> rv(sql_command->read_columnar(nullptr, -1, xsink), xsink);
                purge_messages(xsink);
                return rv.release();
            }

            case command::RES_DONE:
                continue;

            case command::RES_STATUS:
            case command::RES_PARAM:
                xsink->raiseException("COLUMNAR-RESULT-ERROR",
                    "Datasource::selectColumnar() requires an SQL statement returning result columns");
                purge_messages(xsink);
                return nullptr;

            case command::RES_END:
                xsink->raiseException("COLUMNAR-RESULT-ERROR",
                    "Datasource::selectColumnar() requires an SQL statement returning result columns");
                purge_messages(xsink);
                return nullptr;

            case command::RES_ERROR:
            default:
                purge_messages(xsink);
                return nullptr;
        }
    }
}
#endif

QoreValue connection::exec(const QoreString *cmd, const QoreListNode* args, ExceptionSink *xsink) {
    // copy the string here for intrusive editing, convert encoding too if necessary
    QoreString *query = cmd->convertEncoding(enc, xsink);
    if (!query)
        return QoreValue();

    std::unique_ptr<QoreString> tmp(query);
    ValueHolder rv(execReadOutput(query, args, false, true, false, xsink), xsink);
    purge_messages(xsink);
    return rv.release();
}

#ifdef _QORE_HAS_DBI_EXECRAW
QoreValue connection::execRaw(const QoreString *cmd, ExceptionSink *xsink) {
    // copy the string here for intrusive editing, convert encoding too if necessary
    QoreString *query = cmd->convertEncoding(enc, xsink);
    if (!query)
        return QoreValue();

    std::unique_ptr<QoreString> tmp(query);
    ValueHolder rv(execReadOutput(query, 0, false, false, false, xsink), xsink);
    purge_messages(xsink);
    return rv.release();
}
#endif

QoreValue connection::exec_rows(const QoreString *cmd, const QoreListNode *parameters, ExceptionSink *xsink) {
    // copy the string here for intrusive editing, convert encoding too if necessary
    QoreString *query = cmd->convertEncoding(enc, xsink);
    if (!query)
        return QoreValue();

    std::unique_ptr<QoreString> tmp(query);
    ValueHolder rv(execReadOutput(query, parameters, true, true, false, xsink), xsink);
    purge_messages(xsink);
    return rv.release();
}

QoreValue connection::exec_row(const QoreString *cmd, const QoreListNode *parameters, ExceptionSink *xsink) {
    // copy the string here for intrusive editing, convert encoding too if necessary
    QoreString *query = cmd->convertEncoding(enc, xsink);
    if (!query) {
        return QoreValue();
    }

    std::unique_ptr<QoreString> tmp(query);
    ValueHolder rv(execReadOutput(query, parameters, true, true, false, xsink, true), xsink);
    purge_messages(xsink);
    return rv.release();
}

// returns 0=OK, -1=error (exception raised)
int connection::commit(ExceptionSink *xsink) {
#if defined(FREETDS) && defined(HAVE_QORE_BULK_LOAD) && defined(HAVE_FREETDS_BULK)
    if (bulk_load) {
        xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
            "cannot commit while a native FreeTDS bulk-load operation is active");
        return -1;
    }
#endif
    // first clear any pending results in case a statement was in progress (does not clear any actions already effected with exec())
    ct_cancel(m_connection, 0, CS_CANCEL_ALL);
    return direct_execute("commit", xsink);
}

// returns 0=OK, -1=error (exception raised)
int connection::rollback(ExceptionSink *xsink) {
#if defined(FREETDS) && defined(HAVE_QORE_BULK_LOAD) && defined(HAVE_FREETDS_BULK)
    if (bulk_load) {
        xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
            "cannot roll back while a native FreeTDS bulk-load operation is active; abort it first");
        return -1;
    }
#endif
    // first clear any pending results in case a statement was in progress
    ct_cancel(m_connection, 0, CS_CANCEL_ALL);
    return direct_execute("rollback", xsink);
}

#define CHAR_ENC_SIZE 256

// Post-constructor initialization
int connection::init(const char* username,
                     const char* password,
                     const char* dbname,
                     const char* db_encoding,
                     const QoreEncoding* n_enc,
                     const char* hostname,
                     int port,
                     ExceptionSink* xsink) {
    assert(!m_connection);
    printd(5, "connection::init() user: %s pass: %s dbname: %s, db_enc: %s\n", username,
        password ? password : "<n/a>", dbname, db_encoding ? db_encoding : "<n/a>");

    enc = n_enc;

    /*
    // add callbacks
    ret = ct_callback(m_context, 0, CS_SET, CS_CLIENTMSG_CB, (CS_VOID*)clientmsg_callback);
    if (ret != CS_SUCCEED) {
        xsink->raiseException("TDS-CTLIB-SET-CALLBACK", "ct_callback(CS_CLIENTMSG_CB) failed with error %d", ret);
        return -1;
    }
    ret = ct_callback(m_context, 0, CS_SET, CS_SERVERMSG_CB, (CS_VOID*)servermsg_callback);
    if (ret != CS_SUCCEED) {
        xsink->raiseException("TDS-CTLIB-SET-CALLBACK", "ct_callback(CS_SERVERMSG_CB) failed with error %d", ret);
        return -1;
    }
    */

    CS_RETCODE ret = ct_con_alloc(m_context.get_context(), &m_connection);
    if (ret != CS_SUCCEED) {
        xsink->raiseException("TDS-CTLIB-CREATE-CONNECTION", "ct_con_alloc() failed with error %d", ret);
        return -1;
    }

    // set inline message handling
    ret = ct_diag(m_connection, CS_INIT, CS_UNUSED, CS_UNUSED, 0);
    if (ret != CS_SUCCEED) {
        xsink->raiseException("TDS-CTLIB-INIT-ERROR-HANDLING-ERROR", "ct_diag(CS_INIT) failed with error %d, unable to initialize inline error handling", ret);
        return -1;
    }

    // set login timeout to 60 seconds
    CS_INT timeout = 60;
    ret = ct_con_props(m_connection, CS_SET, CS_LOGIN_TIMEOUT, &timeout, CS_UNUSED, 0);
    if (ret != CS_SUCCEED) {
        xsink->raiseException("TDS-CTLIB-SET-LOGIN-TIMEOUT", "ct_con_props(CS_LOGIN_TIMEOUT) failed with error %d", ret);
        return -1;
    }

    ret = ct_con_props(m_connection, CS_SET, CS_USERNAME, (CS_VOID*)username, CS_NULLTERM, 0);
    if (ret != CS_SUCCEED) {
        xsink->raiseException("TDS-CTLIB-SET-USERNAME", "ct_con_props(CS_USERNAME) failed with error %d", ret);
        return -1;
    }
    if (password && password[0]) {
        ret = ct_con_props(m_connection, CS_SET, CS_PASSWORD, (CS_VOID*)password, CS_NULLTERM, 0);
        if (ret != CS_SUCCEED) {
            xsink->raiseException("TDS-CTLIB-SET-PASSWORD", "ct_con_props(CS_PASSWORD) failed with error %d", ret);
            return -1;
        }
    }

    // set hostname and port - when both are provided, we connect directly without freetds.conf
    bool direct_connect = hostname && hostname[0] && port;
    printd(5, "connection::init() hostname=%s port=%d direct_connect=%d dbname=%s\n",
        hostname ? hostname : "NULL", port, direct_connect, dbname ? dbname : "NULL");
    if (direct_connect) {
        QoreString hn(hostname);
        hn.sprintf(" %d", port);

        ret = ct_con_props(m_connection, CS_SET, CS_SERVERADDR, (CS_VOID*)hn.c_str(), CS_NULLTERM, 0);
        if (ret != CS_SUCCEED) {
            xsink->raiseException("TDS-CTLIB-SET-SERVERADDR", "ct_con_props(CS_SERVERADDR, '%s') failed with error %d", hn.c_str(), ret);
            return -1;
        }
    }

#if defined(SYBASE) || defined(FREETDS_LOCALE)
    CS_LOCALE* m_charset_locale = nullptr;

    ret = cs_loc_alloc(m_context.get_context(), &m_charset_locale);
    if (ret != CS_SUCCEED) {
        xsink->raiseException("TDS-EXEC-EXCEPTION", "cs_loc_alloc() returned error %d", (int)ret);
        return -1;
    }
    ON_BLOCK_EXIT(cs_loc_drop, m_context.get_context(), m_charset_locale);
    ret = cs_locale(m_context.get_context(), CS_SET, m_charset_locale, CS_LC_ALL, 0, CS_NULLTERM, 0);
    if (ret != CS_SUCCEED) {
        xsink->raiseException("TDS-EXEC-EXCEPTION", "cs_locale(CS_LC_ALL) returned error %d", (int)ret);
        return -1;
    }
    ret = cs_locale(m_context.get_context(), CS_SET, m_charset_locale, CS_SYB_CHARSET, (CS_CHAR*)db_encoding,
        CS_NULLTERM, 0);
    if (ret != CS_SUCCEED) {
        xsink->raiseException("TDS-EXEC-EXCEPTION", "cs_locale(CS_SYB_CHARSET, '%s') failed with error %d",
            db_encoding, (int)ret);
        return -1;
    }
    ret = ct_con_props(m_connection, CS_SET, CS_LOC_PROP, m_charset_locale, CS_UNUSED, 0);
    if (ret !=CS_SUCCEED) {
        xsink->raiseException("TDS-EXEC-EXCEPTION", "ct_con_props(CS_SET, CS_LOC_PROP) failed with error %d",
            (int)ret);
        return -1;
    }
#endif

#ifndef SYBASE
    // issue #4710: explicitly set the FreeTDS client character set so that FreeTDS
    // performs the correct conversion between the client encoding and the server.
    // The cs_locale()/CS_LOC_PROP mechanism above is not reliable with FreeTDS,
    // which otherwise leaves the client charset at its ISO-8859-1 default; this
    // would corrupt multi-byte (e.g. UTF-8) data sent to/from the server.  This
    // must be set before ct_connect().
    {
        const char* client_cs = enc->getCode();
        ret = ct_con_props(m_connection, CS_SET, CS_CLIENTCHARSET, (CS_VOID*)client_cs, CS_NULLTERM, 0);
        if (ret != CS_SUCCEED) {
            xsink->raiseException("TDS-CTLIB-SET-CLIENTCHARSET",
                "ct_con_props(CS_CLIENTCHARSET, '%s') failed with error %d", client_cs, ret);
            return -1;
        }
    }

    // issue #4321: allow programmatically setting the TDS protocol version so that ad-hoc
    // connections can be made without requiring a freetds.conf entry; this must be set
    // before ct_connect()
    if (!tds_version_set) {
        // read the option from the connect-options hash; setOption() is not called before
        // the connection is opened (private data does not exist yet), so the value set in
        // the datasource configuration / connection string is only available here
        const QoreHashNode* opts = ds->getConnectOptions();
        if (opts) {
            QoreValue v = opts->getKeyValue(SYBASE_OPT_TDS_VERSION);
            if (v) {
                CS_INT ver;
                if (parseTdsVersion(v, ver, xsink)) {
                    return -1;
                }
                tds_version = ver;
                tds_version_set = true;
            }
        }
    }
    if (tds_version_set) {
        printd(5, "connection::init() setting TDS version to %s (%d)\n",
            tdsVersionString(tds_version), (int)tds_version);
        ret = ct_con_props(m_connection, CS_SET, CS_TDS_VERSION, &tds_version, CS_UNUSED, 0);
        if (ret != CS_SUCCEED) {
            xsink->raiseException("TDS-CTLIB-SET-TDS-VERSION", "ct_con_props(CS_TDS_VERSION, '%s') failed with "
                "error %d; the linked FreeTDS client library may not support setting the TDS protocol version "
                "programmatically", tdsVersionString(tds_version), ret);
            return -1;
        }
    }
#endif

    // Check for interrupt before connection
    if (qore_check_cancel(xsink)) {
        return -1;
    }

    // When using direct connection (hostname:port) with CS_SERVERADDR set,
    // we pass dbname as the server identifier (won't be looked up since CS_SERVERADDR is set),
    // then switch to the database with USE command after connecting
    ret = ct_connect(m_connection, (CS_CHAR*)dbname, strlen(dbname));
    if (ret != CS_SUCCEED) {
        do_exception(xsink, "TDS-CTLIB-CONNECT-ERROR", "ct_connect() failed with error %d", ret);
        return -1;
    }
    connected = true;

    // With direct connect, clear any spurious "server not found" warnings that FreeTDS may generate
    if (direct_connect) {
        discard_messages();
    }

    // When using direct connection, switch to the specified database
    if (direct_connect && dbname && dbname[0]) {
        // Validate database name to prevent SQL injection
        // Only allow alphanumeric, underscore, and bracket-delimited names
        QoreString safe_dbname;
        bool needs_quoting = false;
        for (const char* p = dbname; *p; ++p) {
            char c = *p;
            if (!isalnum(c) && c != '_') {
                // Check for already-bracketed name
                if (p == dbname && c == '[') {
                    // Accept bracketed identifiers as-is after validation
                    const char* end = strchr(p + 1, ']');
                    if (end && *(end + 1) == '\0') {
                        safe_dbname.concat(dbname);
                        break;
                    }
                }
                needs_quoting = true;
            }
        }
        if (safe_dbname.empty()) {
            if (needs_quoting) {
                // Quote the identifier with brackets, escaping any existing brackets
                safe_dbname.concat('[');
                for (const char* p = dbname; *p; ++p) {
                    if (*p == ']') {
                        safe_dbname.concat("]]");
                    } else {
                        safe_dbname.concat(*p);
                    }
                }
                safe_dbname.concat(']');
            } else {
                safe_dbname.concat(dbname);
            }
        }

        QoreString use_db("use ");
        use_db.concat(safe_dbname.c_str());
        printd(5, "connection::init() switching to database with: %s\n", use_db.c_str());
        // Clear any pending messages before USE to avoid false errors
        discard_messages();
        if (direct_execute(use_db.c_str(), xsink)) {
            return -1;
        }
    }

    // turn on chained transaction mode, this fits with Qore's transaction management approach
    // - in autocommit mode qore executes a commit after every request manually
    CS_BOOL cs_bool = CS_TRUE;
    ret = ct_options(m_connection, CS_SET, CS_OPT_CHAINXACTS, &cs_bool, CS_UNUSED, 0);
    if (ret != CS_SUCCEED) {
        do_exception(xsink, "TDS-INIT-ERROR", "ct_options(CS_OPT_CHAINXACTS) failed");
    }

    // returns up to 1MB images or text values
    CS_INT cs_size = 1024 * 1024;
    ret = ct_options(m_connection, CS_SET, CS_OPT_TEXTSIZE, &cs_size, CS_UNUSED, 0);
    if (ret != CS_SUCCEED) {
        do_exception(xsink, "TDS-INIT-ERROR", "ct_options(CS_OPT_TEXTSIZE) failed");
    }

    // Set default type of string representation of DATETIME to long (like Jan 1 1990 12:32:55:0000 PM)
    // Without this some routines in conversions.cpp would fail.
    CS_INT aux = CS_DATES_LONG;
    ret = cs_dt_info(m_context.get_context(), CS_SET, NULL, CS_DT_CONVFMT, CS_UNUSED, (CS_VOID*)&aux, sizeof(aux), 0);
    if (ret != CS_SUCCEED) {
        do_exception(xsink, "TDS-INIT-ERROR", "cs_dt_info(CS_DT_CONVFMT) failed");
    }

    // issue #4710: determine DB character encoding
    cs_bool = CS_FALSE;
    ret = ct_con_props(m_connection, CS_SET, CS_CHARSETCNV, &cs_bool, CS_UNUSED, 0);
    if (ret != CS_SUCCEED) {
        do_exception(xsink, "TDS-INIT-ERROR", "ct_options(CS_CHARSETCNV) failed");
    }

    if (!cs_bool) {
        // issue #4710: determine DB character encoding
        // see if we have Sybase or MS SQL
        ValueHolder v(get_server_version(xsink), xsink);
        if (!*xsink) {
            if (v->getType() == NT_STRING) {
                QoreStringValueHelper str(*v);
                if (str->find("Adaptive Server") >= 0) {
                    sybase = true;
                }
            }

            // issue #4710: determine DB character encoding
            if (!sybase) {
                QoreString sql("select convert(varchar, serverproperty('collation')) as 'coll'");
                ValueHolder holder(exec_row(&sql, nullptr, xsink), xsink);
                if (holder->getType() == NT_HASH) {
                    // issue #4710 / qore#4321: for MS SQL Server, use UTF-8 as the
                    // connection encoding and let FreeTDS perform the character set
                    // conversion.  With the TDS 7.x protocol used for MS SQL, FreeTDS
                    // transparently converts between the UTF-8 client charset and the
                    // server representation for *all* string column types:
                    //  - NCHAR/NVARCHAR/NTEXT: UCS-2 on the wire (full Unicode), and
                    //  - CHAR/VARCHAR/TEXT: the column's (server collation) code page.
                    // Forcing the connection encoding to the server's single-byte
                    // code page (e.g. WINDOWS-1252 for the default
                    // SQL_Latin1_General_CP1_CI_AS collation) makes it impossible to
                    // store/retrieve Unicode in N-type columns and is incorrect when
                    // FreeTDS is doing the conversion; UTF-8 works for every column
                    // type and every server collation (legacy LOB types like TEXT/
                    // NTEXT also keep working, whereas a server-wide _UTF8 collation
                    // would break them).
                    enc = QCS_UTF8;
                    mssql = true;
                    printd(5, "MS SQL Server detected; using UTF-8 connection "
                        "encoding (FreeTDS handles server-side conversion)\n");
                }
            }
        }
    }

    purge_messages(xsink);
    return *xsink ? -1 : 0;
}

void connection::discard_messages() {
#ifdef DEBUG
    CS_RETCODE ret =
#endif
        ct_diag(m_connection, CS_CLEAR, CS_ALLMSG_TYPE, CS_UNUSED, 0);
    assert(ret == CS_SUCCEED);
}

// purges all outstanding messages using ct_diag
int connection::purge_messages(ExceptionSink *xsink) {
    int rc = 0;
    // make sure no messages have severity > 10
    int num;
#ifdef DEBUG
    CS_RETCODE ret =
#endif
        ct_diag(m_connection, CS_STATUS, CS_CLIENTMSG_TYPE, CS_UNUSED, &num);
    assert(ret == CS_SUCCEED);
    CS_CLIENTMSG cmsg;
    for (int i = 1; i <= num; ++i) {
#ifdef DEBUG
        ret =
#endif
            ct_diag(m_connection, CS_GET, CS_CLIENTMSG_TYPE, i, &cmsg);
        assert(ret == CS_SUCCEED);
        if (CS_SEVERITY(cmsg.msgnumber) > 10) {
            QoreStringNode *desc = new QoreStringNode;
            desc->sprintf("client message %d, severity %d: %s", CS_NUMBER(cmsg.msgnumber), CS_SEVERITY(cmsg.msgnumber), cmsg.msgstring);
            if (cmsg.osstringlen)
                desc->sprintf(" (%d): '%s'", cmsg.osnumber, cmsg.osstring);
            xsink->raiseException("TDS-CLIENT-ERROR", desc);
            rc = -1;
        }
#ifdef DEBUG
        printd(1, "client: severity:%d, n:%d: %s\n", CS_SEVERITY(cmsg.msgnumber), CS_NUMBER(cmsg.msgnumber), cmsg.msgstring);
        if (cmsg.osstringlen > 0)
            printd(1, "Operating System Error: %s\n", cmsg.osstring);
#endif
    }
#ifdef DEBUG
    ret =
#endif
        ct_diag(m_connection, CS_STATUS, CS_SERVERMSG_TYPE, CS_UNUSED, &num);
    assert(ret == CS_SUCCEED);
    CS_SERVERMSG smsg;
    for (int i = 1; i <= num; ++i) {
#ifdef DEBUG
        ret =
#endif
            ct_diag(m_connection, CS_GET, CS_SERVERMSG_TYPE, i, &smsg);
        assert(ret == CS_SUCCEED);
        if (smsg.severity > 10) {
            QoreStringNode *desc = new QoreStringNode;
            desc->sprintf("state %d, server message %d, ", smsg.state, smsg.msgnumber);
            if (smsg.line)
                desc->sprintf("line %d, ", smsg.line);
            desc->sprintf("severity %ld: %s", smsg.severity, smsg.text);
            desc->trim_trailing('\n');
            xsink->raiseException("TDS-SERVER-ERROR", desc);
            rc = -1;
        }
        printd(1, "server: line:%ld, severity:%ld, n:%d: %s", smsg.line, smsg.severity, smsg.msgnumber, smsg.text);
    }
#ifdef DEBUG
    ret =
#endif
        ct_diag(m_connection, CS_CLEAR, CS_ALLMSG_TYPE, CS_UNUSED, 0);
    assert(ret == CS_SUCCEED);
    return rc;
}

// if check = true, ignore server messages:
// 3902: The COMMIT TRANSACTION request has no corresponding BEGIN TRANSACTION
// 3903: The ROLLBACK TRANSACTION request has no corresponding BEGIN TRANSACTION
void connection::do_check_exception(ExceptionSink *xsink, bool check, const char *err, QoreStringNode* estr) {
    int count = 0;
    int num;
    CS_RETCODE ret = ct_diag(m_connection, CS_STATUS, CS_CLIENTMSG_TYPE, CS_UNUSED, &num);
    assert(ret == CS_SUCCEED);
    CS_CLIENTMSG cmsg;
    for (int i = 1; i <= num; ++i) {
        ret = ct_diag(m_connection, CS_GET, CS_CLIENTMSG_TYPE, i, &cmsg);
        if (ret != CS_SUCCEED)
            continue;
        int severity = CS_SEVERITY(cmsg.msgnumber);
        if (severity <= 10)
            continue;

        if (count)
            estr->concat(", ");
        estr->sprintf("client message %d: severity %d: %s", CS_NUMBER(cmsg.msgnumber), severity, cmsg.msgstring);
        estr->trim_trailing('.');
        if (cmsg.osnumber && cmsg.osstringlen > 0)
            estr->sprintf(", OS error %d: %s", cmsg.osnumber, cmsg.osstring);
        count++;
    }

    ret = ct_diag(m_connection, CS_STATUS, CS_SERVERMSG_TYPE, CS_UNUSED, &num);
    assert(ret == CS_SUCCEED);
    CS_SERVERMSG smsg;

    bool fnd_ignore = !check;

    for (int i = 1; i <= num; ++i) {
        ret = ct_diag(m_connection, CS_GET, CS_SERVERMSG_TYPE, i, &smsg);
        if (!fnd_ignore && ret == CS_SUCCEED && (smsg.msgnumber == 3902 || smsg.msgnumber == 3903))
        fnd_ignore = true;

        if (ret != CS_SUCCEED || smsg.severity <= 10)
            continue;

        if (count)
            estr->concat(", ");
        if (smsg.svrnlen)
            estr->sprintf("%s: ", smsg.svrname);
        estr->sprintf("state %d, server message %d, ", smsg.state, smsg.msgnumber);
        if (smsg.line)
            estr->sprintf("line %d, ", smsg.line);
        estr->sprintf("severity %d", smsg.severity);
        if (smsg.textlen)
            estr->sprintf(": %s", smsg.text);
        estr->trim_trailing("\n.");
        ++count;
    }
    ret = ct_diag(m_connection, CS_CLEAR, CS_ALLMSG_TYPE, CS_UNUSED, 0);
    assert(ret == CS_SUCCEED);
    if (check && fnd_ignore && num == 1)
        return;

    if (!count)
        estr->terminate(estr->size() - 2);

    throw ss::Error(err, estr->c_str());
}

void connection::do_check_exception(ExceptionSink *xsink, bool check, const char *err, const char *fmt, ...) {
    SimpleRefHolder<QoreStringNode> estr(new QoreStringNode);
    va_list args;
    while (fmt) {
        va_start(args, fmt);
        int rc = estr->vsprintf(fmt, args);
        va_end(args);
        if (!rc) {
            estr->concat(": ");
            break;
        }
    }
    do_check_exception(xsink, check, err, *estr);
}

void connection::do_exception(ExceptionSink *xsink, const char *err, const char *fmt, ...) {
    SimpleRefHolder<QoreStringNode> estr(new QoreStringNode);
    va_list args;
    while (fmt) {
        va_start(args, fmt);
        int rc = estr->vsprintf(fmt, args);
        va_end(args);
        if (!rc)
            break;
    }
    estr->concat(": ");
    do_check_exception(xsink, false, err, *estr);
}

/*
  CS_RETCODE connection::clientmsg_callback(CS_CONTEXT* ctx, CS_CONNECTION* conn, CS_CLIENTMSG* errmsg)
  {
  #ifdef DEBUG
  // This will print out description about an Sybase error. Most of the information
  // can be ignored but if the application crashes the last error may be very informative.
  // Comment it out if this output is not required.
  if ((CS_NUMBER(errmsg->msgnumber) == 211) || (CS_NUMBER(errmsg->msgnumber) == 212)) { // acc. to the docs
  return CS_SUCCEED;
  }
  fprintf(stderr, "-------------------------------------------------------------");
  fprintf(stderr, "\nOpen Client Message:\n");
  fprintf(stderr, "Message number: LAYER = (%d) ORIGIN = (%d) ",
  (int)CS_LAYER(errmsg->msgnumber), (int)CS_ORIGIN(errmsg->msgnumber));
  fprintf(stderr, "SEVERITY = (%d) NUMBER = (%d)\n",
  (int)CS_SEVERITY(errmsg->msgnumber), (int)CS_NUMBER(errmsg->msgnumber));
  fprintf(stderr, "Message String: %s\n", errmsg->msgstring);
  if (errmsg->osstringlen > 0) {
  fprintf(stderr, "Operating System Error: %s\n", errmsg->osstring);
  }
  fprintf(stderr, "--------------------------------------------------\n");
  fflush(stderr);
  #endif
  return CS_SUCCEED;
  }

  CS_RETCODE connection::servermsg_callback(CS_CONTEXT* ctx, CS_CONNECTION* conn, CS_SERVERMSG* svrmsg)
  {
  #ifdef DEBUG
  // This will print out description about an Sybase error. Most of the information
  // can be ignored but if the application crashes the last error may be very informative.
  // Comment it out if this output is not required.
  fprintf(stderr, "-------------------------------------------------------------");
  fprintf(stderr, "\nOpen Server Message:\n");
  fprintf(stderr, "Message number = %d, severity = %d\n", (int)svrmsg->msgnumber, (int)svrmsg->severity);
  fprintf(stderr, "State = %d, line = %d\n", (int)svrmsg->state, (int)svrmsg->line);
  if (svrmsg->svrnlen) {
  fprintf(stderr, "Server: %s\n", svrmsg->svrname);
  }
  if (svrmsg->proclen) {
  fprintf(stderr, "Procedure: %s\n", svrmsg->proc);
  }
  fprintf(stderr, "Message string: %s\n", svrmsg->text);
  fprintf(stderr, "--------------------------------------------------\n");
  fflush(stderr);
  #endif
  return CS_SUCCEED;
  }
*/

// get client version
QoreStringNode *connection::get_client_version(ExceptionSink *xsink) {
   return m_context.get_client_version(xsink);
}

QoreValue connection::get_server_version(ExceptionSink *xsink) {
    QoreValue rv;
    {
        ValueHolder res(execReadOutput(&ver_str, 0, true, true, false, xsink), xsink);
        if (!res) {
            return QoreValue();
        }
        assert(res->getType() == NT_HASH);
        HashIterator hi(res->get<QoreHashNode>());
        hi.next();
        rv = hi.removeKeyValue();
    }

    if (rv.getType() == NT_STRING) {
        QoreStringNodeValueHelper str(rv);
        QoreStringNode* trimmed = str.getReferencedValue();
        rv.discard(xsink);
        rv = trimmed;
        trimmed->trim_trailing('\n');
    }

    return rv;
}

#ifndef SYBASE
// issue #4321: mapping between "tds-version" option values and FreeTDS CS_TDS_* constants
namespace {
struct TdsVersionMap {
    const char* str;
    CS_INT ver;
};

// the first string for each CS_TDS_* value is the canonical form returned by getOption()
static const TdsVersionMap tds_version_map[] = {
    {"auto",  CS_TDS_AUTO},
    {"0",     CS_TDS_AUTO},
    {"4.0",   CS_TDS_40},
    {"40",    CS_TDS_40},
    {"4.2",   CS_TDS_42},
    {"42",    CS_TDS_42},
    {"4.6",   CS_TDS_46},
    {"46",    CS_TDS_46},
    {"4.9.5", CS_TDS_495},
    {"4.95",  CS_TDS_495},
    {"495",   CS_TDS_495},
    {"5.0",   CS_TDS_50},
    {"5",     CS_TDS_50},
    {"50",    CS_TDS_50},
    {"7.0",   CS_TDS_70},
    {"7",     CS_TDS_70},
    {"70",    CS_TDS_70},
    {"7.1",   CS_TDS_71},
    {"71",    CS_TDS_71},
    {"7.2",   CS_TDS_72},
    {"72",    CS_TDS_72},
    {"7.3",   CS_TDS_73},
    {"73",    CS_TDS_73},
    {"7.4",   CS_TDS_74},
    {"74",    CS_TDS_74},
};
}

int connection::parseTdsVersion(QoreValue val, CS_INT& ver, ExceptionSink* xsink) {
    // the option is registered with stringTypeInfo, so the value is normally a string;
    // convert defensively to support numeric values as well (e.g. "7.0" parsed as a float)
    QoreStringValueHelper str(val);
    QoreString v(*str);
    v.trim();
    v.tolwr();
    for (const TdsVersionMap& m : tds_version_map) {
        if (!strcmp(v.c_str(), m.str)) {
            ver = m.ver;
            return 0;
        }
    }
    xsink->raiseException("TDS-OPTION-ERROR", "invalid value '%s' for the '%s' option; expecting one of: "
        "\"auto\", \"4.0\", \"4.2\", \"4.6\", \"4.9.5\", \"5.0\", \"7.0\", \"7.1\", \"7.2\", \"7.3\", \"7.4\"",
        v.c_str(), SYBASE_OPT_TDS_VERSION);
    return -1;
}

const char* connection::tdsVersionString(CS_INT ver) {
    for (const TdsVersionMap& m : tds_version_map) {
        if (m.ver == ver) {
            return m.str;
        }
    }
    return nullptr;
}
#endif

DLLLOCAL int connection::setOption(const char* opt, QoreValue val, ExceptionSink* xsink) {
    if (!strcasecmp(opt, DBI_OPT_NUMBER_OPT)) {
        numeric_support = OPT_NUM_OPTIMAL;
        return 0;
    }

    if (!strcasecmp(opt, DBI_OPT_NUMBER_STRING)) {
        numeric_support = OPT_NUM_STRING;
        return 0;
    }

    if (!strcasecmp(opt, DBI_OPT_NUMBER_NUMERIC)) {
        numeric_support = OPT_NUM_NUMERIC;
        return 0;
    }

    if (!strcasecmp(opt, DBI_OPT_TIMEZONE)) {
        assert(val.getType() == NT_STRING);
        QoreStringValueHelper str(val);
        const AbstractQoreZoneInfo* tz =
            find_create_timezone(str->c_str(), xsink);
        if (*xsink) return -1;
        server_tz = tz;
        return 0;
    }

    if (!strcasecmp(opt, SYBASE_OPT_OPTIMIZED_DATE_BINDS)) {
        optimized_date_binds = true;
        return 0;
    }

#ifndef SYBASE
    if (!strcasecmp(opt, SYBASE_OPT_TDS_VERSION)) {
        CS_INT ver;
        if (parseTdsVersion(val, ver, xsink)) {
            return -1;
        }
        tds_version = ver;
        tds_version_set = true;
        // note: takes effect on the next (re)connection
        return 0;
    }
#endif

    assert(false);
    return 0;
}

DLLLOCAL QoreValue connection::getOption(const char* opt) {
    if (!strcasecmp(opt, DBI_OPT_NUMBER_OPT)) {
        return numeric_support == OPT_NUM_OPTIMAL;
    }

    if (!strcasecmp(opt, DBI_OPT_NUMBER_STRING)) {
        return numeric_support == OPT_NUM_STRING;
    }

    if (!strcasecmp(opt, DBI_OPT_NUMBER_NUMERIC)) {
        return numeric_support == OPT_NUM_NUMERIC;
    }

    if (!strcasecmp(opt, DBI_OPT_TIMEZONE)) {
        return new QoreStringNode(tz_get_region_name(getTZ()));
    }

    if (!strcasecmp(opt, SYBASE_OPT_OPTIMIZED_DATE_BINDS)) {
        return optimized_date_binds;
    }

#ifndef SYBASE
    if (!strcasecmp(opt, SYBASE_OPT_TDS_VERSION)) {
        if (!tds_version_set) {
            return QoreValue();
        }
        const char* str = tdsVersionString(tds_version);
        return str ? new QoreStringNode(str) : QoreValue();
    }
#endif

    assert(false);
    return QoreValue();
}

DLLLOCAL const AbstractQoreZoneInfo* connection::getTZ() const {
    return server_tz ? server_tz : currentTZ();
}
