/* -*- mode: c++; indent-tabs-mode: nil -*- */

#ifndef _QORE_SYBASE_STATEMENT_H_

#define _QORE_SYBASE_STATEMENT_H_

#include "config.h"

#include <qore/common.h>
#include <qore/QoreValue.h>
#include <qore/DBI.h>
#include <qore/QoreEncoding.h>
#include <qore/QoreString.h>
#include <qore/SQLStatement.h>
#include <qore/QoreStringNode.h>
#include <qore/AbstractPrivateData.h>
#include <qore/QoreQueue.h>
#include <qore/Datasource.h>
#include <qore/QoreListNode.h>
#include <qore/QoreHashNode.h>

#include "connection.h"
#include "emptystatement.h"
#include "command.h"
#include "dbmodulewrap.h"
#include "utils.h"

namespace ss {

inline bool expect_row(command::ResType rt) {
    switch (rt) {
        case command::RES_ROW:
        case command::RES_PARAM:
            return true;
        default:
            return false;
    }
}

class Statement : public EmptyStatement {
    QoreHashNode *_execRes;

    bool has_res() {
        return _execRes ? true : false;
    }

    void set_res(QoreHashNode *res, ExceptionSink* xsink) {
        if (_execRes) _execRes->deref(xsink);
        _execRes = res;
    }

    QoreHashNode * release_res() {
        QoreHashNode *rv = _execRes;
        _execRes = 0;
        return rv;
    }

   DLLLOCAL int checkValid(ExceptionSink* xsink) {
      if (valid)
         return 0;
      xsink->raiseException("STATEMENT-CONNECTION-ERROR", "the connection was lost while the statement was in progress; it cannot be used any longer");
      return -1;
   }

    ~Statement() {}

    SafePtr<command> context;
    Placeholders placeholders;
    bool valid;
    //! true when every result of the execution has been read
    bool results_done = false;

    //! reads the results after the statement's result set when its rows have been read to the end
    /** @return 0 = OK, -1 = error (exception raised)
    */
    DLLLOCAL int finishIfEnded(SQLStatement* stmt, ExceptionSink* xsink) {
        if (results_done || !context->result_ended()) {
            return 0;
        }
        return finishResults(stmt, xsink);
    }

    //! reads the row count and return status results before the first result set
    /** without a result set, every result is read

        @return 0 = OK, -1 = error (exception raised)
    */
    DLLLOCAL int positionOnResultSet(connection* conn, ExceptionSink* xsink);

    //! reads the results after the statement's result set
    /** row counts are added to the affected row count, the error of a later statement is raised, and another result
        set raises \c TDS-MULTIPLE-RESULT-SETS-ERROR; on error the remaining results are discarded

        @return 0 = OK, -1 = error (exception raised)
    */
    DLLLOCAL int finishResults(SQLStatement* stmt, ExceptionSink* xsink);

    //! discards the results that have not been read, with their messages
    DLLLOCAL void discardResults(connection& conn);

public:
    typedef connection Connection;

    Statement() : _execRes(0), valid(true) {}

    DLLLOCAL static void Delete(Statement *self, ExceptionSink* xsink);

    DLLLOCAL void invalidate() {
        assert(valid);
        //printd(5, "Statement::invalidate() context: %p\n", &*context);
        context->cancelDisconnect();
        valid = false;
    }

    DLLLOCAL int exec(connection *conn, const QoreString *query, const QoreListNode *args, bool raw, ExceptionSink* xsink);

    DLLLOCAL bool next(SQLStatement* stmt, ExceptionSink* xsink);

    DLLLOCAL bool isValid() const {
        return valid;
    }

    QoreHashNode * fetch_row(SQLStatement* stmt, ExceptionSink* xsink) {
        return release_res();
    }

    int define(SQLStatement* stmt, ExceptionSink* xsink) {
        return 0;
    }

    QoreListNode* fetch_rows(SQLStatement* stmt, int rows, ExceptionSink* xsink) {
        if (checkValid(xsink))
            return 0;
        ReferenceHolder<QoreListNode> reslist(new QoreListNode(autoTypeInfo), xsink);

        // rows <= 0 means all rows; a row read by next() and not yet returned is returned first, and no row is read
        // after the last one returned
        while (rows <= 0 || reslist->size() < static_cast<size_t>(rows)) {
            // next() reads the remaining results, and raises the error of a later statement, at the end of the rows
            if (!next(stmt, xsink)) {
                break;
            }
            reslist->push(release_res(), xsink);
            if (!(reslist->size() % 100) && qore_check_cancel(xsink)) {
                return nullptr;
            }
        }
        if (*xsink) {
            return nullptr;
        }
        return reslist.release();
    }

    QoreHashNode* get_output(SQLStatement* stmt, ExceptionSink* xsink) {
        if (checkValid(xsink))
           return 0;
        ReferenceHolder<QoreHashNode> rv(context->read_cols(&placeholders, false, xsink), xsink);
        if (*xsink || finishIfEnded(stmt, xsink)) {
            return nullptr;
        }
        return rv.release();
    }

    int affected_rows(SQLStatement* stmt, ExceptionSink* xsink);

    QoreHashNode* fetch_columns(SQLStatement* stmt, int rows, ExceptionSink* xsink) {
        if (checkValid(xsink))
           return 0;
        ReferenceHolder<QoreHashNode> rv(context->read_cols(0, rows, false, xsink), xsink);
        if (*xsink || finishIfEnded(stmt, xsink)) {
            return nullptr;
        }
        return rv.release();
    }

#ifdef HAVE_QORE_COLUMNAR_RESULT
    QoreColumnarResult* fetch_columnar(SQLStatement* stmt, int rows, ExceptionSink* xsink) {
        if (checkValid(xsink))
           return 0;
        ReferenceHolder<QoreColumnarResult> rv(context->read_columnar(0, rows, xsink), xsink);
        if (*xsink || finishIfEnded(stmt, xsink)) {
            return nullptr;
        }
        return rv.release();
    }
#endif

    int bind_placeholders(SQLStatement* stmt, const QoreListNode& l, ExceptionSink* xsink) {
        if (checkValid(xsink))
           return -1;
        placeholders.clear();
        ConstListIterator it(l);
        while (it.next()) {
            QoreValue v(it.getValue());
            QoreStringValueHelper s(v);
            placeholders.emplace_back(s->c_str(), s->size());
        }
        return 0;
    }
};

void init(qore_dbi_method_list &methods);

} // namespace ss

#endif
