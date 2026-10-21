/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    command.h

    Sybase DB layer for QORE
    uses Sybase OpenClient C library

    Qore Programming language

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

#ifndef SYBASE_COMMAND_H_
#define SYBASE_COMMAND_H_

#include "config.h"

#include <ctpublic.h>

#include <memory>

#include "sybase_query.h"
#include "row_output_buffers.h"
#include "conversions.h"
#include "utils.h"

#ifdef HAVE_QORE_COLUMNAR_RESULT
class QoreColumnarResult;
#endif
class connection;

struct CS_DATAFMT_EX : public CS_DATAFMT {
    int origin_datatype;
    //! the maximum length that the server described, before the driver adjusted it for its buffers
    CS_INT origin_maxlength;
};

typedef std::vector<CS_DATAFMT_EX> row_result_t;

class Columns {
public:
    Columns() : dirty(true) {}

    bool dirty;
    row_result_t datafmt;

    size_t count() { return datafmt.size(); }
    bool empty() { return datafmt.empty(); }

    void reset() {
        datafmt.clear();
        dirty = true;
    }

    bool need_refresh() {
        return dirty || empty();
    }

    void set_dirty() { dirty = true; }
};

class command {
public:
    enum ResType {
        RES_NONE,
        RES_PARAM,
        RES_STATUS,
        RES_ROW,
        RES_END,
        RES_DONE,
        RES_ERROR,
        RES_RETRY,
        RES_CANCELED,
    };

    DLLLOCAL ResType read_next_result(bool& disconnect, ExceptionSink* xsink) {
        /**
        Can't ask for result if rastRes==:
        * STATUS, PARAM or ROW - must ct_fetch() the values first
        * END or ERROR - reasonable, we're finished
        * RETRY - never gets here
        */
        while ((lastRes = read_next_result1(disconnect, xsink)) == RES_RETRY) {}
        return lastRes;
    }

    DLLLOCAL void cancelDisconnect() {
        // the command has already been dropped if the connection was lost while it was being read
        if (!m_cmd) {
            return;
        }
        //printd(5, "command::cancelIntern() %d this: %p m_cmd: %p\n", cancelIntern(), this, m_cmd);
        cancelIntern();
        ct_cmd_drop(m_cmd);
        m_cmd = 0;
        lastRes = RES_CANCELED;
    }

    DLLLOCAL QoreHashNode* fetch_row(ExceptionSink* xsink, const Placeholders *ph = 0);

    DLLLOCAL int cancel() {
        if (lastRes == RES_END)
            return 0;

        lastRes = RES_CANCELED;
        return m_cmd ? cancelIntern() : 0;
    }

    DLLLOCAL int get_row_count();

    //! returns the total of the row counts read so far, or -1 if the server reported none
    /** the row count reported at the end of a result set (the number of rows selected) is not included
    */
    DLLLOCAL int get_affected_rows() const;

    //! returns true if the rows of the current result have been read to the end
    DLLLOCAL bool result_ended() const {
        return lastRes == RES_NONE;
    }

    //! returns true if every result has been read, or the command has been canceled
    DLLLOCAL bool results_read() const {
        return lastRes == RES_END || lastRes == RES_CANCELED;
    }

    //! reads and discards the rows of the current result (a return status or output parameter result)
    /** @return 0 = OK, -1 = error (exception raised)
    */
    DLLLOCAL int skip_result(ExceptionSink* xsink);

    //! reads the results after a result set: row counts are added, return status and output parameter results are
    //! skipped, and another result set raises \c TDS-MULTIPLE-RESULT-SETS-ERROR
    /** @param conn the connection
        @param desc describes the SQL for the exception, ex: "SQL passed to selectRows()"
        @param xsink for errors of later statements and the exception above

        @return 0 = all results have been read, -1 = error (exception raised)
    */
    DLLLOCAL int finish_results(connection& conn, const char* desc, ExceptionSink* xsink);

    //! raises \c TDS-MULTIPLE-RESULT-SETS-ERROR for a call that returns one result set
    /** @param desc describes the SQL, ex: "SQL passed to selectRows()"
    */
    DLLLOCAL static void raise_multiple_result_sets(const char* desc, ExceptionSink* xsink);

    DLLLOCAL command(connection& conn, ExceptionSink* xsink);
    DLLLOCAL ~command();

    DLLLOCAL void clear();

    DLLLOCAL CS_COMMAND* operator()() const { return m_cmd; }
    DLLLOCAL connection& getConnection() const { return m_conn; }

    DLLLOCAL void send(ExceptionSink* xsink);
    DLLLOCAL void initiate_language_command(const char *cmd_text, class ExceptionSink *xsink);
    // returns true if data returned, false if not
    DLLLOCAL bool fetch_row_into_buffers(class ExceptionSink *xsink);
    // returns the number of columns in the result
    DLLLOCAL unsigned get_column_count(ExceptionSink *xsink);
    // returns 0=OK, -1=error (exception raised)
    DLLLOCAL void set_params(sybase_query &query, const QoreListNode *args, ExceptionSink *xsink);

    //! reads every result of the command
    /** @param only_result_set if not null, the call returns one result set: row counts are skipped, and a second
        result set raises \c TDS-MULTIPLE-RESULT-SETS-ERROR; the value describes the SQL for the exception, ex:
        "SQL passed to selectRows()"; without a result set, the total row count is returned (if greater than 0)
    */
    DLLLOCAL QoreValue readOutput(connection& conn, command& cmd, bool list, bool& connection_reset, bool cols,
            ExceptionSink* xsink, bool single_row = false, const char* only_result_set = nullptr);

    DLLLOCAL QoreHashNode *read_cols(const Placeholders *placeholder_list,
                                    int cnt,
                                    bool cols,
                                    ExceptionSink* xsink);

#ifdef HAVE_QORE_COLUMNAR_RESULT
    DLLLOCAL QoreColumnarResult* read_columnar(const Placeholders* placeholder_list, int cnt, ExceptionSink* xsink);
#endif

    //! describes the columns of the current result, or of the last result set when every result has been read
    /** @return a hash keyed by column name, as the rows are returned, with a description hash for each column:
        \c name, \c type (the type code of the value returned), \c maxsize, \c native_type, \c internal_id (the
        Client-Library datatype), \c nullable, and \c precision and \c scale for \c DECIMAL and \c NUMERIC columns;
        an empty hash if no result has columns
    */
    DLLLOCAL QoreHashNode* describe(const Placeholders* placeholder_list, ExceptionSink* xsink);

    DLLLOCAL QoreHashNode *read_cols(const Placeholders *placeholder_list,
                                    bool cols,
                                    ExceptionSink* xsink) {
    return read_cols(placeholder_list, -1, cols, xsink);
    }

    DLLLOCAL QoreValue read_rows(Placeholders *placeholder_list, bool list, bool cols, ExceptionSink* xsink, bool single_row = false);
    DLLLOCAL QoreValue read_rows(const Placeholders *placeholder_list, ExceptionSink* xsink, bool single_row = false);

    DLLLOCAL void set_placeholders(const Placeholders &ph) {
        query->placeholders = ph;
    }

    DLLLOCAL int bind_query(std::unique_ptr<sybase_query> &query,
                            const QoreListNode *args,
                            ExceptionSink*);

private:
    ss::SafePtr<sybase_query> query;

    connection& m_conn;
    CS_COMMAND* m_cmd;
    CS_INT rowcount;
    ResType lastRes;
    //! the total of the row counts read
    int64 total_rowcount = 0;
    //! true if the server reported a row count
    bool rowcount_reported = false;
    //! true after a row or output parameter result: the row count reported at its end is not added
    bool after_result = false;

    Columns colinfo;
    row_output_buffers out_buffers;

    DLLLOCAL int retr_colinfo(ExceptionSink* xsink);

    DLLLOCAL int ensure_colinfo(ExceptionSink* xsink) {
        // after the last result, the columns of the last result set are kept: a fetch returns no rows
        if (!colinfo.need_refresh() || (results_read() && !colinfo.empty()))
        return 0;
        return retr_colinfo(xsink);
    }

    // returns 0=OK, -1=error (exception raised)
    DLLLOCAL int get_row_description(row_result_t &result, unsigned column_count, class ExceptionSink *xsink);
    DLLLOCAL int setup_output_buffers(const row_result_t &input_row_descriptions, class ExceptionSink *xsink) {
        return setup_output_buffers(input_row_descriptions, out_buffers, xsink);
    }
    DLLLOCAL int setup_output_buffers(const row_result_t& input_row_descriptions, row_output_buffers& buffers,
            ExceptionSink* xsink);

    DLLLOCAL int append_buffers_to_list(row_result_t &column_info, row_output_buffers& all_buffers, class QoreHashNode *h, ExceptionSink* xsink);

    DLLLOCAL QoreHashNode* output_buffers_to_hash(const Placeholders* ph, ExceptionSink* xsink);
    DLLLOCAL QoreValue get_value(const CS_DATAFMT_EX& datafmt, const output_value_buffer& buffer, ExceptionSink* xsink);

    // call ct_result() once. Takes care of return value
    DLLLOCAL ResType read_next_result1(bool& disconnect, ExceptionSink* xsink);

    // returns -1 if the cancel failed and the connection should be disconnected, 0 = OK
    DLLLOCAL int cancelIntern() {
        assert(m_cmd);
        return ct_cancel(0, m_cmd, CS_CANCEL_ALL) == CS_FAIL ? -1 : 0;
    }

    DLLLOCAL QoreValue getNumber(const char* str, size_t len);

    DLLLOCAL void setupColumns(QoreHashNode& h, const Placeholders *ph);

    //! returns the name of the given column, as the rows are returned
    DLLLOCAL std::string get_column_name(size_t i, const Placeholders* ph) const;

    //! assigns the value to the column name in the hash, with a suffix if the name is already used
    DLLLOCAL static void assign_column(QoreHashNode& h, const std::string& col_name, QoreValue value,
            ExceptionSink* xsink);
};


#endif
