/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    bulk_load.cpp

    Qore Programming Language

    Copyright (C) 2026 Qore Technologies, s.r.o.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the
    Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#if defined(FREETDS) && defined(HAVE_QORE_BULK_LOAD) && defined(HAVE_FREETDS_BULK)

#include "bulk_load.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "connection.h"
#include "encoding_helpers.h"

static constexpr int64 QORE_FREETDS_STREAM_REPORT_BYTES = 65536;
static constexpr CS_INT QORE_FREETDS_MAX_TABLE_COLUMNS = 1024;
static constexpr const char* QORE_FREETDS_BULK_SAVEPOINT = "qore_native_bulk_load";

static std::string qoreSybaseLowerIdentifier(const char* value, size_t size) {
    std::string result(value, size);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

static bool qoreSybaseGetBulkType(const std::string& type_name, QoreSybaseBulkType& type) {
    if (type_name == "char" || type_name == "varchar" || type_name == "text") {
        type = QoreSybaseBulkType::Text;
    } else if (type_name == "nchar" || type_name == "nvarchar" || type_name == "ntext") {
        type = QoreSybaseBulkType::Unicode;
    } else if (type_name == "binary" || type_name == "varbinary" || type_name == "image") {
        type = QoreSybaseBulkType::Binary;
    } else if (type_name == "tinyint" || type_name == "smallint" || type_name == "int"
        || type_name == "bigint") {
        type = QoreSybaseBulkType::Integer;
    } else if (type_name == "decimal" || type_name == "numeric" || type_name == "money"
        || type_name == "smallmoney") {
        type = QoreSybaseBulkType::Numeric;
    } else if (type_name == "float" || type_name == "real") {
        type = QoreSybaseBulkType::Floating;
    } else if (type_name == "bit") {
        type = QoreSybaseBulkType::Boolean;
    } else if (type_name == "date" || type_name == "datetime" || type_name == "datetime2"
        || type_name == "smalldatetime" || type_name == "time") {
        type = QoreSybaseBulkType::DateTime;
    } else {
        return false;
    }
    return true;
}

//! Validates an unquoted SQL Server identifier with at most two qualifiers.
static bool qoreSybaseParseIdentifier(const char* value, bool qualified, ExceptionSink* xsink) {
    if (!value || !*value) {
        return false;
    }
    bool at_start = true;
    unsigned parts = 1;
    size_t part_size = 0;
    size_t count = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p, ++count) {
        if (count && !(count % 100)
            && qore_check_cancel(xsink, "FreeTDS native bulk-load identifier validation")) {
            return false;
        }
        if (*p == '.') {
            if (!qualified || at_start || !p[1] || parts == 2) {
                return false;
            }
            ++parts;
            at_start = true;
            part_size = 0;
            continue;
        }
        if (at_start) {
            if (!(std::isalpha(*p) || *p == '_')) {
                return false;
            }
            at_start = false;
        } else if (!(std::isalnum(*p) || *p == '_' || *p == '$' || *p == '#' || *p == '@')) {
            return false;
        }
        if (++part_size > 128) {
            return false;
        }
    }
    return !at_start;
}

static int qoreSybaseGetBoolOption(const QoreHashNode* options, const char* name, bool default_value,
        bool& result, ExceptionSink* xsink) {
    result = default_value;
    if (!options) {
        return 0;
    }
    QoreValue value = options->getKeyValue(name);
    if (value.isNothing()) {
        return 0;
    }
    if (value.getType() != NT_BOOLEAN) {
        xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
            "native FreeTDS bulk-load option '%s' must be boolean", name);
        return -1;
    }
    result = value.getAsBool();
    return 0;
}

QoreSybaseBulkLoadState::QoreSybaseBulkLoadState(connection& conn, bool stream_bounds)
        : conn(conn), ds(conn.getDatasource()),
          stream_active(stream_bounds && ds->sqlMutationObserverActive()) {
}

QoreSybaseBulkLoadState::~QoreSybaseBulkLoadState() {
    emergencyAbort();
    if (stream_started) {
        ExceptionSink xsink;
        endStream(false, &xsink);
    }
}

void QoreSybaseBulkLoadState::emergencyAbort() {
    if (!descriptor && !savepoint) {
        return;
    }
    ExceptionSink xsink;
    cancelAndRollback(&xsink, false);
}

int QoreSybaseBulkLoadState::cancelAndRollback(ExceptionSink* xsink, bool report_errors) {
    int rc = 0;
    if (descriptor) {
        // FreeTDS 1.x implements CS_BLK_CANCEL as a no-op, and CS_BLK_BATCH immediately starts
        // another BCP exchange.  Complete the current exchange before rolling back the savepoint;
        // otherwise SQL Server reports a truncated TDS stream and leaves the connection unusable.
        // Any rows completed here are removed by the rollback below.
        ExceptionSink cleanup_xsink;
        if (protocol_started) {
            CS_INT ignored = 0;
            CS_RETCODE done_rc;
            {
                QoreSybaseCancelHelper cancel_helper(conn.getConnection());
                done_rc = blk_done(descriptor, CS_BLK_ALL, &ignored);
            }
            conn.purge_messages(&cleanup_xsink);
            if (done_rc != CS_SUCCEED) {
                CS_RETCODE cancel_rc = ct_cancel(conn.getConnection(), nullptr, CS_CANCEL_ALL);
                if (cancel_rc != CS_SUCCEED && !cleanup_xsink) {
                    cleanup_xsink.raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "ct_cancel() failed while aborting native FreeTDS bulk loading with error %d", cancel_rc);
                }
            }
        }
        CS_RETCODE drop_rc = blk_drop(descriptor);
        descriptor = nullptr;
        if (drop_rc != CS_SUCCEED && !cleanup_xsink) {
            cleanup_xsink.raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "blk_drop() failed while aborting native FreeTDS bulk loading with error %d", drop_rc);
        }
        if (cleanup_xsink) {
            rc = -1;
            if (report_errors && !*xsink) {
                xsink->assimilate(cleanup_xsink);
            }
        }
    }
    if (savepoint) {
        savepoint = false;
        QoreStringMaker rollback("rollback transaction %s", QORE_FREETDS_BULK_SAVEPOINT);
        ExceptionSink local_xsink;
        ExceptionSink* rollback_xsink = report_errors ? xsink : &local_xsink;
        if (conn.direct_execute(rollback.c_str(), rollback_xsink)) {
            rc = -1;
        }
    }
    return rc;
}

int QoreSybaseBulkLoadState::initialize(const QoreString* table, const QoreListNode* column_list,
        ExceptionSink* xsink) {
    if (!qoreSybaseParseIdentifier(table->c_str(), true, xsink)) {
        return *xsink ? -1 : 1;
    }
    if (column_list->size() > QORE_FREETDS_MAX_TABLE_COLUMNS) {
        xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
            "SQL Server native bulk loading supports at most %d columns; got %zu",
            QORE_FREETDS_MAX_TABLE_COLUMNS, column_list->size());
        return -1;
    }

    std::set<std::string> requested;
    ConstListIterator li(column_list);
    while (li.next()) {
        if (columns.size() && !(columns.size() % 100)
            && qore_check_cancel(xsink, "FreeTDS native bulk-load column validation")) {
            return -1;
        }
        QoreValue value = li.getValue();
        if (value.getType() != NT_STRING) {
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "native FreeTDS bulk-load column %zu has type '%s'; expected string",
                columns.size() + 1, value.getTypeName());
            return -1;
        }
        // note: column names are short enough to be held in inline short string storage (ex:
        // "id"), which has no QoreStringNode, so the data helper must be used to read the bytes
        QoreStringDataHelper name(value);
        if (!qoreSybaseParseIdentifier(name.c_str(), false, xsink)) {
            return *xsink ? -1 : 1;
        }
        std::string normalized = qoreSybaseLowerIdentifier(name.c_str(), name.size());
        if (!requested.insert(normalized).second) {
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "column '%s' occurs more than once in the native FreeTDS bulk-load column list", name.c_str());
            return -1;
        }
        columns.push_back({std::string(name.c_str(), name.size())});
    }

    // The CT-Library metadata returned by blk_describe() does not distinguish SQL Server
    // VARCHAR from NVARCHAR reliably.  Read the server catalog before starting the BCP protocol
    // so Unicode values can be bound as UTF-16LE and non-insertable generated columns can be
    // excluded accurately.
    QoreStringMaker metadata_query(
        "select c.column_id ordinal, c.name column_name, lower(type_name(c.system_type_id)) type_name, "
        "c.max_length max_length, c.is_nullable is_nullable, c.is_identity is_identity, "
        "c.is_computed is_computed, c.generated_always_type generated_always_type "
        "from sys.columns c where c.object_id = object_id('%s') order by c.column_id", table->c_str());
    ValueHolder metadata_rows(conn.exec_rows(&metadata_query, nullptr, xsink), xsink);
    if (*xsink) {
        return -1;
    }
    if (metadata_rows->getType() != NT_LIST && metadata_rows->getType() != NT_HASH) {
        return 1;
    }

    struct MetadataColumn {
        CS_INT ordinal;
        CS_INT max_length;
        QoreSybaseBulkType type;
        std::string type_name;
        bool nullable;
    };
    std::unordered_map<std::string, MetadataColumn> metadata;
    size_t insertable_column_count = 0;
    auto add_metadata = [&](const QoreHashNode* row) -> int {
        QoreValue ordinal_value = row->getKeyValue("ordinal");
        QoreValue name_value = row->getKeyValue("column_name");
        QoreValue type_value = row->getKeyValue("type_name");
        QoreValue max_length_value = row->getKeyValue("max_length");
        if (ordinal_value.getType() != NT_INT || name_value.getType() != NT_STRING
            || type_value.getType() != NT_STRING || max_length_value.getType() != NT_INT) {
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "SQL Server returned invalid column metadata for native FreeTDS bulk loading");
            return -1;
        }
        bool identity = row->getKeyValue("is_identity").getAsBool();
        bool computed = row->getKeyValue("is_computed").getAsBool();
        bool generated = row->getKeyValue("generated_always_type").getAsBigInt() != 0;
        // note: these values can be held in inline short string storage, which has no
        // QoreStringNode, so the data helper must be used to read the bytes
        QoreStringDataHelper type(type_value);
        std::string type_name(type.c_str(), type.size());
        if (identity || computed || generated || type_name == "timestamp" || type_name == "rowversion") {
            return 0;
        }
        ++insertable_column_count;
        QoreStringDataHelper name(name_value);
        std::string normalized = qoreSybaseLowerIdentifier(name.c_str(), name.size());
        CS_INT ordinal = static_cast<CS_INT>(ordinal_value.getAsBigInt());
        if (ordinal < 1 || ordinal > QORE_FREETDS_MAX_TABLE_COLUMNS) {
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "SQL Server returned invalid native bulk-load column ordinal %d", ordinal);
            return -1;
        }
        QoreSybaseBulkType bulk_type;
        if (!qoreSybaseGetBulkType(type_name, bulk_type)) {
            return 0;
        }
        int64 max_length = max_length_value.getAsBigInt();
        if (max_length < -1 || max_length > std::numeric_limits<CS_INT>::max()) {
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "SQL Server returned invalid maximum length %lld for native bulk-load column '%s'",
                static_cast<long long>(max_length), name.c_str());
            return -1;
        }
        bool nullable = row->getKeyValue("is_nullable").getAsBool();
        metadata.emplace(std::move(normalized), MetadataColumn{ordinal, static_cast<CS_INT>(max_length),
            bulk_type, std::move(type_name), nullable});
        return 0;
    };
    if (metadata_rows->getType() == NT_HASH) {
        if (add_metadata(metadata_rows->get<const QoreHashNode>())) {
            return -1;
        }
    } else {
        ConstListIterator metadata_iter(metadata_rows->get<const QoreListNode>());
        size_t metadata_count = 0;
        while (metadata_iter.next()) {
            if (metadata_count && !(metadata_count % 100)
                && qore_check_cancel(xsink, "indexing SQL Server native bulk-load metadata")) {
                return -1;
            }
            ++metadata_count;
            QoreValue row_value = metadata_iter.getValue();
            if (row_value.getType() != NT_HASH) {
                xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                    "SQL Server returned invalid row metadata for native FreeTDS bulk loading");
                return -1;
            }
            if (add_metadata(row_value.get<const QoreHashNode>())) {
                return -1;
            }
        }
    }

    // FreeTDS's CT-Library bulk API has no implemented default-column binding.  Native loading
    // is therefore eligible only when every insertable table column is supplied; otherwise the
    // ordinary insert path preserves defaults and generated values.
    if (metadata.empty() || metadata.size() != insertable_column_count
        || insertable_column_count != requested.size()) {
        return 1;
    }
    size_t mapped_count = 0;
    for (QoreSybaseBulkColumn& column : columns) {
        if (mapped_count && !(mapped_count % 100)
            && qore_check_cancel(xsink, "mapping SQL Server native bulk-load columns")) {
            return -1;
        }
        ++mapped_count;
        auto iter = metadata.find(qoreSybaseLowerIdentifier(column.key.c_str(), column.key.size()));
        if (iter == metadata.end()) {
            return 1;
        }
        column.ordinal = iter->second.ordinal;
        column.max_length = iter->second.max_length;
        column.type = iter->second.type;
        column.type_name = iter->second.type_name;
        column.nullable = iter->second.nullable;
    }

    QoreStringMaker save("if @@trancount = 0 begin transaction; save transaction %s",
        QORE_FREETDS_BULK_SAVEPOINT);
    if (conn.direct_execute(save.c_str(), xsink)) {
        return -1;
    }
    savepoint = true;

    if (qore_check_cancel(xsink, "initializing FreeTDS native bulk loading")) {
        cancelAndRollback(xsink, true);
        return -1;
    }
    CS_RETCODE ret = blk_alloc(conn.getConnection(), BLK_VERSION_100, &descriptor);
    if (ret != CS_SUCCEED) {
        xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
            "blk_alloc() failed while initializing native bulk loading with error %d", ret);
        cancelAndRollback(xsink, true);
        return -1;
    }
    {
        QoreSybaseCancelHelper cancel_helper(conn.getConnection());
        ret = blk_init(descriptor, CS_BLK_IN, const_cast<CS_CHAR*>(table->c_str()),
            static_cast<CS_INT>(table->size()));
    }
    if (ret != CS_SUCCEED) {
        conn.purge_messages(xsink);
        if (!*xsink) {
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "blk_init() failed for table '%s' with error %d", conn.maskMessage(table->c_str()).c_str(), ret);
        }
        cancelAndRollback(xsink, true);
        return -1;
    }

    cells.resize(columns.size());

    if (stream_active) {
        if (ds->reportMutationStreamBegin(0, xsink)) {
            failed = true;
            cancelAndRollback(xsink, true);
            return -1;
        }
        stream_started = true;
    }
    return 0;
}

int QoreSybaseBulkLoadState::serialize(QoreValue value, const QoreSybaseBulkColumn& column,
        QoreSybaseBulkCell& cell, ExceptionSink* xsink) {
    cell.data.clear();
    cell.length = 0;
    cell.indicator = CS_GOODDATA;
    cell.format = {};
    cell.format.count = 1;
    cell.format.format = CS_FMT_UNUSED;
    cell.format.status = CS_INPUTVALUE;

    bool textual = false;
    bool unicode = column.type == QoreSybaseBulkType::Unicode;
    if (value.isNullOrNothing()) {
        if (!column.nullable) {
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "null cannot be loaded into non-nullable native FreeTDS bulk-load column '%s'",
                column.key.c_str());
            return -1;
        }
        cell.indicator = CS_NULLDATA;
        cell.format.datatype = unicode ? CS_UNICHAR_TYPE : CS_CHAR_TYPE;
    } else {
        qore_type_t value_type = value.getType();
        bool compatible = false;
        switch (column.type) {
            case QoreSybaseBulkType::Text:
            case QoreSybaseBulkType::Unicode:
                compatible = value_type == NT_STRING;
                break;
            case QoreSybaseBulkType::Binary:
                compatible = value_type == NT_BINARY;
                break;
            case QoreSybaseBulkType::Integer:
                compatible = value_type == NT_INT;
                break;
            case QoreSybaseBulkType::Numeric:
                compatible = value_type == NT_INT || value_type == NT_NUMBER;
                break;
            case QoreSybaseBulkType::Floating:
                compatible = value_type == NT_INT || value_type == NT_FLOAT || value_type == NT_NUMBER;
                break;
            case QoreSybaseBulkType::Boolean:
                compatible = value_type == NT_BOOLEAN;
                break;
            case QoreSybaseBulkType::DateTime:
                compatible = value_type == NT_DATE;
                break;
        }
        if (!compatible) {
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "Qore type '%s' is not supported for SQL Server %s column '%s' in native FreeTDS bulk loading",
                value.getTypeName(), column.type_name.c_str(), column.key.c_str());
            return -1;
        }
        if (column.type == QoreSybaseBulkType::Integer) {
            int64 number = value.getAsBigInt();
            bool in_range = column.type_name == "bigint"
                || (column.type_name == "int" && number >= std::numeric_limits<int32_t>::min()
                    && number <= std::numeric_limits<int32_t>::max())
                || (column.type_name == "smallint" && number >= std::numeric_limits<int16_t>::min()
                    && number <= std::numeric_limits<int16_t>::max())
                || (column.type_name == "tinyint" && number >= 0 && number <= 255);
            if (!in_range) {
                xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                    "integer %lld is outside the range of SQL Server %s column '%s'",
                    static_cast<long long>(number), column.type_name.c_str(), column.key.c_str());
                return -1;
            }
        }
        switch (value.getType()) {
            case NT_BOOLEAN:
                cell.data.push_back(value.getAsBool() ? '1' : '0');
                textual = true;
                break;
            case NT_INT: {
                char buffer[32];
                int count = snprintf(buffer, sizeof(buffer), QLLD, value.getAsBigInt());
                if (count <= 0 || static_cast<size_t>(count) >= sizeof(buffer)) {
                    xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "could not format a native FreeTDS bulk-load integer");
                    return -1;
                }
                cell.data.assign(buffer, buffer + count);
                textual = true;
                break;
            }
            case NT_FLOAT: {
                double number = value.getAsFloat();
                if (!std::isfinite(number)) {
                    xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "non-finite floats are not supported by native FreeTDS bulk loading");
                    return -1;
                }
                char buffer[64];
                int count = snprintf(buffer, sizeof(buffer), "%.17g", number);
                if (count <= 0 || static_cast<size_t>(count) >= sizeof(buffer)) {
                    xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "could not format a native FreeTDS bulk-load float");
                    return -1;
                }
                cell.data.assign(buffer, buffer + count);
                textual = true;
                break;
            }
            case NT_NUMBER: {
                QoreStringValueHelper string(value);
                cell.data.assign(string->c_str(), string->c_str() + string->size());
                textual = true;
                break;
            }
            case NT_STRING: {
                QoreStringValueHelper string(value);
                TempEncodingHelper encoded(*string, unicode ? QCS_UTF16LE : conn.getEncoding(), xsink);
                if (!encoded) {
                    return -1;
                }
                const unsigned char* begin = reinterpret_cast<const unsigned char*>(encoded->getBuffer());
                cell.data.assign(begin, begin + encoded->size());
                if (column.max_length >= 0 && (column.type_name == "nchar" || column.type_name == "nvarchar")
                    && cell.data.size() > static_cast<size_t>(column.max_length)) {
                    xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "string payload for native FreeTDS bulk-load column '%s' is %zu bytes; maximum is %d",
                        column.key.c_str(), cell.data.size(), column.max_length);
                    return -1;
                }
                cell.format.datatype = unicode ? CS_UNICHAR_TYPE : CS_CHAR_TYPE;
                break;
            }
            case NT_BINARY: {
                const BinaryNode* binary = value.get<const BinaryNode>();
                if (binary->size()) {
                    const unsigned char* begin = static_cast<const unsigned char*>(binary->getPtr());
                    cell.data.assign(begin, begin + binary->size());
                }
                if (column.max_length >= 0 && (column.type_name == "binary" || column.type_name == "varbinary")
                    && cell.data.size() > static_cast<size_t>(column.max_length)) {
                    xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "binary payload for native FreeTDS bulk-load column '%s' is %zu bytes; maximum is %d",
                        column.key.c_str(), cell.data.size(), column.max_length);
                    return -1;
                }
                cell.format.datatype = CS_BINARY_TYPE;
                break;
            }
            case NT_DATE: {
                const DateTimeNode* date = value.get<const DateTimeNode>();
                if (date->isRelative()) {
                    xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "relative date/time values are not supported by native FreeTDS bulk loading");
                    return -1;
                }
                qore_tm info;
                date->getInfo(conn.getTZ(), info);
                if (info.year < 1 || info.year > 9999) {
                    xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "native FreeTDS bulk-load date/time values must have a year from 1 through 9999; got %d",
                        info.year);
                    return -1;
                }
                if (column.type_name == "datetime" && info.year < 1753) {
                    xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "native FreeTDS bulk-load datetime values must be from 1753 through 9999; got year %d",
                        info.year);
                    return -1;
                }
                if (column.type_name == "smalldatetime" && (info.year < 1900 || info.year > 2079)) {
                    xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "native FreeTDS bulk-load smalldatetime values must be from 1900 through 2079; got year %d",
                        info.year);
                    return -1;
                }
                char buffer[32];
                int count;
                if (column.type_name == "date") {
                    count = snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", info.year, info.month, info.day);
                } else if (column.type_name == "time") {
                    count = snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%06d",
                        info.hour, info.minute, info.second, info.us);
                } else {
                    count = snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%06d",
                        info.year, info.month, info.day, info.hour, info.minute, info.second, info.us);
                }
                if (count <= 0 || static_cast<size_t>(count) >= sizeof(buffer)) {
                    xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "could not format a native FreeTDS bulk-load date/time value");
                    return -1;
                }
                cell.data.assign(buffer, buffer + count);
                textual = true;
                break;
            }
            default:
                xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                    "Qore type '%s' is not supported by native FreeTDS bulk loading", value.getTypeName());
                return -1;
        }
    }

    if (textual) {
        cell.format.datatype = CS_CHAR_TYPE;
    }

    if (cell.data.size() > static_cast<size_t>(std::numeric_limits<CS_INT>::max())) {
        xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
            "a native FreeTDS bulk-load value is too large for CT-Library (%zu bytes)", cell.data.size());
        return -1;
    }
    cell.length = static_cast<CS_INT>(cell.data.size());
    cell.format.maxlength = std::max<CS_INT>(cell.length, 1);
    if (cell.data.empty()) {
        cell.data.push_back(0);
    }
    return 0;
}

int QoreSybaseBulkLoadState::addBytes(size_t bytes, ExceptionSink* xsink) {
    if (bytes > static_cast<size_t>(std::numeric_limits<int64>::max() - consumed)) {
        failed = true;
        stream_ok = false;
        xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
            "native FreeTDS bulk-load payload byte counter overflow");
        return -1;
    }
    consumed += static_cast<int64>(bytes);
    if (!stream_started || consumed - reported < QORE_FREETDS_STREAM_REPORT_BYTES) {
        return 0;
    }
    reported = consumed;
    if (ds->reportMutationStreamProgress(consumed, xsink)) {
        failed = true;
        stream_ok = false;
        return -1;
    }
    return 0;
}

int QoreSybaseBulkLoadState::sendRows(const QoreHashNode* rows, ExceptionSink* xsink) {
    if (failed) {
        xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
            "the active native FreeTDS bulk-load operation has already failed; abort it");
        return -1;
    }
    size_t row_count = 0;
    bool have_row_count = false;
    std::vector<QoreValue> values;
    values.reserve(columns.size());
    size_t validated_count = 0;
    for (const QoreSybaseBulkColumn& column : columns) {
        if (validated_count && !(validated_count % 100)
            && qore_check_cancel(xsink, "validating FreeTDS native bulk-load row columns")) {
            failed = true;
            stream_ok = false;
            return -1;
        }
        ++validated_count;
        if (!rows->existsKey(column.key.c_str())) {
            failed = true;
            stream_ok = false;
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "native FreeTDS bulk-load row block has no value for column '%s'", column.key.c_str());
            return -1;
        }
        QoreValue value = rows->getKeyValue(column.key.c_str());
        if (value.getType() == NT_LIST) {
            const QoreListNode* list = value.get<const QoreListNode>();
            if (!have_row_count) {
                row_count = list->size();
                have_row_count = true;
            } else if (row_count != list->size()) {
                failed = true;
                stream_ok = false;
                xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                    "native FreeTDS bulk-load column '%s' has %zu rows; expected %zu",
                    column.key.c_str(), list->size(), row_count);
                return -1;
            }
        }
        values.push_back(value);
    }
    if (!have_row_count) {
        row_count = 1;
    }
    if (!row_count) {
        return 0;
    }

    for (size_t row = 0; row < row_count; ++row) {
        if (!(row % 100) && qore_check_cancel(xsink, "sending FreeTDS native bulk-load rows")) {
            failed = true;
            stream_ok = false;
            return -1;
        }
        size_t row_bytes = 0;
        for (size_t column = 0; column < columns.size(); ++column) {
            if (column && !(column % 100)
                && qore_check_cancel(xsink, "serializing FreeTDS native bulk-load row columns")) {
                failed = true;
                stream_ok = false;
                return -1;
            }
            QoreValue value = values[column];
            if (value.getType() == NT_LIST) {
                value = value.get<const QoreListNode>()->retrieveEntry(row);
            }
            if (serialize(value, columns[column], cells[column], xsink)) {
                failed = true;
                stream_ok = false;
                return -1;
            }
            row_bytes += static_cast<size_t>(cells[column].length);
            CS_RETCODE ret = blk_bind(descriptor, columns[column].ordinal, &cells[column].format,
                cells[column].data.data(), &cells[column].length, &cells[column].indicator);
            if (ret != CS_SUCCEED) {
                failed = true;
                stream_ok = false;
                conn.purge_messages(xsink);
                if (!*xsink) {
                    xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                        "blk_bind() failed for column '%s' with error %d", columns[column].key.c_str(), ret);
                }
                return -1;
            }
        }
        CS_RETCODE ret;
        {
            QoreSybaseCancelHelper cancel_helper(conn.getConnection());
            protocol_started = true;
            ret = blk_rowxfer(descriptor);
        }
        if (ret != CS_SUCCEED) {
            failed = true;
            stream_ok = false;
            conn.purge_messages(xsink);
            if (!*xsink) {
                xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                    "blk_rowxfer() failed for row %zu with error %d", row + 1, ret);
            }
            return -1;
        }
        if (addBytes(row_bytes, xsink)) {
            return -1;
        }
        ++rows_sent;
    }
    return 0;
}

int QoreSybaseBulkLoadState::endStream(bool success, ExceptionSink* xsink) {
    if (!stream_started) {
        return 0;
    }
    stream_started = false;
    stream_ok = stream_ok && success;
    return ds->reportMutationStreamEnd(consumed, stream_ok, xsink);
}

int QoreSybaseBulkLoadState::finish(bool success, ExceptionSink* xsink) {
    int rc = 0;
    bool native_success = success && !failed;
    if (native_success && !protocol_started) {
        CS_RETCODE drop_rc = blk_drop(descriptor);
        descriptor = nullptr;
        if (drop_rc != CS_SUCCEED) {
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "blk_drop() failed after an empty native FreeTDS bulk load with error %d", drop_rc);
            native_success = false;
            stream_ok = false;
            rc = -1;
        } else {
            savepoint = false;
        }
    }
    if (native_success && protocol_started) {
        CS_INT copied = 0;
        CS_RETCODE ret;
        {
            QoreSybaseCancelHelper cancel_helper(conn.getConnection());
            ret = blk_done(descriptor, CS_BLK_ALL, &copied);
        }
        int message_rc = conn.purge_messages(xsink);
        CS_RETCODE drop_rc = CS_SUCCEED;
        if (ret == CS_SUCCEED) {
            // CS_BLK_ALL deinitializes the descriptor's BCP state, so release it here even when
            // SQL Server reported a row error or the copied-row count is inconsistent.
            drop_rc = blk_drop(descriptor);
            descriptor = nullptr;
        }
        if (ret != CS_SUCCEED || message_rc) {
            failed = true;
            stream_ok = false;
            native_success = false;
            if (!*xsink) {
                xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                    "blk_done(CS_BLK_ALL) failed with error %d", ret);
            }
            rc = -1;
        } else if (rows_sent > static_cast<uint64_t>(std::numeric_limits<CS_INT>::max())
            || copied != static_cast<CS_INT>(rows_sent)) {
            failed = true;
            stream_ok = false;
            native_success = false;
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "FreeTDS reported %d rows copied after sending %llu native rows", copied,
                static_cast<unsigned long long>(rows_sent));
            rc = -1;
        } else if (drop_rc != CS_SUCCEED) {
            xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
                "blk_drop() failed after native FreeTDS bulk loading with error %d", drop_rc);
            native_success = false;
            stream_ok = false;
            rc = -1;
        } else {
            savepoint = false;
        }
    }
    if (!native_success && cancelAndRollback(xsink, true)) {
        rc = -1;
    }
    if (endStream(native_success, xsink)) {
        rc = -1;
    }
    return *xsink ? -1 : rc;
}

int connection::bulkLoadBegin(const QoreString* table, const QoreListNode* columns,
        const QoreHashNode* options, ExceptionSink* xsink) {
    if (bulk_load) {
        xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
            "a native FreeTDS bulk-load operation is already active");
        return -1;
    }
    if (!isMsSql()) {
        return 1;
    }
    bool stream_bounds;
    if (qoreSybaseGetBoolOption(options, "stream_bounds", true, stream_bounds, xsink)) {
        return -1;
    }
    std::unique_ptr<QoreSybaseBulkLoadState> state(new QoreSybaseBulkLoadState(*this, stream_bounds));
    int rc = state->initialize(table, columns, xsink);
    if (!rc) {
        bulk_load = std::move(state);
    }
    return rc;
}

int connection::bulkLoadRows(const QoreHashNode* rows, ExceptionSink* xsink) {
    if (!bulk_load) {
        xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
            "no native FreeTDS bulk-load operation is active");
        return -1;
    }
    return bulk_load->sendRows(rows, xsink);
}

int connection::bulkLoadEnd(bool success, ExceptionSink* xsink) {
    if (!bulk_load) {
        xsink->raiseException("DBI:FREETDS:BULK-LOAD-ERROR",
            "no native FreeTDS bulk-load operation is active");
        return -1;
    }
    std::unique_ptr<QoreSybaseBulkLoadState> state = std::move(bulk_load);
    return state->finish(success, xsink);
}

#endif
