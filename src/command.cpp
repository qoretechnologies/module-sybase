/* -*- indent-tabs-mode: nil -*- */
/*
    command.cpp

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

#include <assert.h>
#include <cstypes.h>

#include <cerrno>
#include <cctype>
#include <cstring>
#include <memory>
#include <iostream>
#include <algorithm>
#include <string>
#include <vector>

#include "sybase.h"

#ifdef HAVE_QORE_COLUMNAR_RESULT
#include <qore/QoreBufferNode.h>
#include <qore/QoreColumnarResult.h>
#endif

#include "command.h"
#include "connection.h"
#include "utils.h"
#include "resultfactory.h"

#ifdef HAVE_QORE_COLUMNAR_RESULT
namespace {
struct SybaseColumnarStorage {
    std::vector<int64> int_values;
    std::vector<double> float_values;
    std::vector<QoreBufferDecimal128> decimal_values;
    std::vector<uint8_t> validity;
};

static size_t sybase_columnar_bitmap_size(size_t size) {
    return (size + 7) / 8;
}

static void sybase_columnar_set_validity_bit(std::vector<uint8_t>& validity, size_t index, bool valid) {
    size_t byte = index / 8;
    if (byte >= validity.size()) {
        validity.resize(byte + 1, 0);
    }

    uint8_t mask = uint8_t(1) << (index % 8);
    if (valid) {
        validity[byte] |= mask;
    } else {
        validity[byte] &= ~mask;
    }
}

static bool sybase_columnar_decimal_metadata_supported(const CS_DATAFMT_EX& datafmt) {
    return (datafmt.origin_datatype == CS_DECIMAL_TYPE || datafmt.origin_datatype == CS_NUMERIC_TYPE)
        && datafmt.precision > 0 && datafmt.precision <= 38 && datafmt.scale >= 0
        && datafmt.scale <= datafmt.precision;
}

static __int128 sybase_columnar_decimal_abs(__int128 value) {
    return value < 0 ? -value : value;
}

static __int128 sybase_columnar_decimal_pow10(int32_t exponent) {
    __int128 rv = 1;
    for (int32_t i = 0; i < exponent; ++i) {
        rv *= 10;
    }
    return rv;
}

static int32_t sybase_columnar_decimal_precision(__int128 value) {
    value = sybase_columnar_decimal_abs(value);
    int32_t rv = 1;
    while (value >= 10) {
        value /= 10;
        ++rv;
    }
    return rv;
}

static QoreBufferDecimal128 sybase_columnar_decimal_storage(__int128 value) {
    unsigned __int128 bits = static_cast<unsigned __int128>(value);
    return QoreBufferDecimal128{static_cast<uint64_t>(bits), static_cast<int64_t>(bits >> 64)};
}

static bool sybase_columnar_parse_int64(const char* input, int64& value) {
    if (!input || !*input || strchr(input, '.') || strchr(input, 'e') || strchr(input, 'E')) {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    long long rv = strtoll(input, &end, 10);
    if (errno == ERANGE || !end || *end) {
        return false;
    }

    value = static_cast<int64>(rv);
    return true;
}

static int sybase_columnar_parse_decimal128(const char* input, int32_t target_precision, int32_t target_scale,
        const char* column_name, QoreBufferDecimal128& out, ExceptionSink* xsink) {
    assert(target_precision > 0 && target_precision <= 38 && target_scale >= 0 && target_scale <= target_precision);
    if (!input) {
        xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
            "cannot convert DECIMAL(%d,%d) column '%s' to decimal128; value is not available",
            target_precision, target_scale, column_name);
        return -1;
    }

    size_t begin = 0;
    size_t input_size = strlen(input);
    while (begin < input_size && std::isspace(static_cast<unsigned char>(input[begin]))) {
        ++begin;
    }

    size_t end = input_size;
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    if (begin == end) {
        xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
            "cannot convert DECIMAL(%d,%d) column '%s' empty value to decimal128",
            target_precision, target_scale, column_name);
        return -1;
    }

    bool negative = false;
    size_t pos = begin;
    if (input[pos] == '+' || input[pos] == '-') {
        negative = input[pos] == '-';
        ++pos;
    }

    bool seen_digit = false;
    bool seen_dot = false;
    int64_t fractional_digits = 0;
    std::string digits;
    for (; pos < end; ++pos) {
        unsigned char c = static_cast<unsigned char>(input[pos]);
        if (std::isdigit(c)) {
            seen_digit = true;
            digits.push_back(static_cast<char>(c));
            if (seen_dot) {
                ++fractional_digits;
            }
            continue;
        }
        if (input[pos] == '.' && !seen_dot) {
            seen_dot = true;
            continue;
        }
        break;
    }

    if (!seen_digit) {
        xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
            "cannot convert DECIMAL(%d,%d) column '%s' value '%s' to decimal128; expected at least one digit",
            target_precision, target_scale, column_name, input);
        return -1;
    }

    int64_t exponent = 0;
    if (pos < end && (input[pos] == 'e' || input[pos] == 'E')) {
        ++pos;
        bool exponent_negative = false;
        if (pos < end && (input[pos] == '+' || input[pos] == '-')) {
            exponent_negative = input[pos] == '-';
            ++pos;
        }
        if (pos == end || !std::isdigit(static_cast<unsigned char>(input[pos]))) {
            xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
                "cannot convert DECIMAL(%d,%d) column '%s' value '%s' to decimal128; invalid exponent",
                target_precision, target_scale, column_name, input);
            return -1;
        }
        while (pos < end && std::isdigit(static_cast<unsigned char>(input[pos]))) {
            exponent = (exponent * 10) + (input[pos] - '0');
            if (exponent > 76) {
                xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
                    "cannot convert DECIMAL(%d,%d) column '%s' value '%s' to decimal128; exponent is too large",
                    target_precision, target_scale, column_name, input);
                return -1;
            }
            ++pos;
        }
        if (exponent_negative) {
            exponent = -exponent;
        }
    }

    if (pos != end) {
        xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
            "cannot convert DECIMAL(%d,%d) column '%s' value '%s' to decimal128; unexpected character '%c'",
            target_precision, target_scale, column_name, input, input[pos]);
        return -1;
    }

    int64_t source_scale = fractional_digits - exponent;
    if (source_scale < 0) {
        digits.append(static_cast<size_t>(-source_scale), '0');
        source_scale = 0;
    }

    size_t first_non_zero = digits.find_first_not_of('0');
    if (first_non_zero == std::string::npos) {
        out = sybase_columnar_decimal_storage(0);
        return 0;
    }

    size_t significant_digits = digits.size() - first_non_zero;
    if (significant_digits > 38) {
        xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
            "cannot convert DECIMAL(%d,%d) column '%s' value '%s' to decimal128; precision %zu"
            " exceeds decimal128 maximum precision 38", target_precision, target_scale, column_name, input,
            significant_digits);
        return -1;
    }

    __int128 unscaled = 0;
    for (size_t i = first_non_zero; i < digits.size(); ++i) {
        unscaled = (unscaled * 10) + (digits[i] - '0');
    }
    if (negative) {
        unscaled = -unscaled;
    }

    if (source_scale < target_scale) {
        int64_t scale_delta = target_scale - source_scale;
        if (sybase_columnar_decimal_precision(unscaled) + scale_delta > target_precision) {
            xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
                "cannot convert DECIMAL(%d,%d) column '%s' value '%s' to decimal128; value exceeds declared precision",
                target_precision, target_scale, column_name, input);
            return -1;
        }
        unscaled *= sybase_columnar_decimal_pow10(static_cast<int32_t>(scale_delta));
    } else if (source_scale > target_scale) {
        int64_t scale_delta = source_scale - target_scale;
        if (scale_delta > 38) {
            xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
                "cannot convert DECIMAL(%d,%d) column '%s' value '%s' to decimal128 without losing precision",
                target_precision, target_scale, column_name, input);
            return -1;
        }

        __int128 divisor = sybase_columnar_decimal_pow10(static_cast<int32_t>(scale_delta));
        if (unscaled % divisor) {
            xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
                "cannot convert DECIMAL(%d,%d) column '%s' value '%s' to decimal128 without losing precision",
                target_precision, target_scale, column_name, input);
            return -1;
        }
        unscaled /= divisor;
    }

    if (sybase_columnar_decimal_precision(unscaled) > target_precision) {
        xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
            "cannot convert DECIMAL(%d,%d) column '%s' value '%s' to decimal128; value exceeds declared precision",
            target_precision, target_scale, column_name, input);
        return -1;
    }

    out = sybase_columnar_decimal_storage(unscaled);
    return 0;
}

enum class SybaseColumnarKind {
    Int64,
    Float64,
    Decimal128,
    List,
};

class SybaseColumnarBuilder {
public:
    SybaseColumnarBuilder(const CS_DATAFMT_EX& datafmt, const char* n_name, int numeric_option, ExceptionSink* xsink)
        : name(n_name), list(xsink) {
        switch (datafmt.datatype) {
            case CS_TINYINT_TYPE:
            case CS_SMALLINT_TYPE:
            case CS_INT_TYPE:
#ifdef CS_BIGINT_TYPE
            case CS_BIGINT_TYPE:
#endif
                kind = SybaseColumnarKind::Int64;
                storage.reset(new SybaseColumnarStorage);
                break;

            case CS_REAL_TYPE:
            case CS_FLOAT_TYPE:
                kind = SybaseColumnarKind::Float64;
                storage.reset(new SybaseColumnarStorage);
                break;

            case CS_CHAR_TYPE:
                if (numeric_option != connection::OPT_NUM_STRING && sybase_columnar_decimal_metadata_supported(datafmt)) {
                    precision = datafmt.precision;
                    scale = datafmt.scale;
                    kind = numeric_option == connection::OPT_NUM_OPTIMAL && !scale && precision <= 18
                        ? SybaseColumnarKind::Int64
                        : SybaseColumnarKind::Decimal128;
                    storage.reset(new SybaseColumnarStorage);
                    break;
                }
                [[fallthrough]];

            default:
                kind = SybaseColumnarKind::List;
                list = new QoreListNode(autoTypeInfo);
                break;
        }
    }

    const char* getName() const {
        return name.c_str();
    }

    SybaseColumnarKind getKind() const {
        return kind;
    }

    int appendNull() {
        ensureValidity();
        sybase_columnar_set_validity_bit(storage->validity, row_count, false);
        switch (kind) {
            case SybaseColumnarKind::Float64:
                storage->float_values.push_back(0.0);
                break;
            case SybaseColumnarKind::Decimal128:
                storage->decimal_values.push_back(QoreBufferDecimal128{0, 0});
                break;
            default:
                storage->int_values.push_back(0);
                break;
        }
        ++null_count;
        ++row_count;
        return 0;
    }

    int appendInt64(int64 value) {
        storage->int_values.push_back(value);
        appendValid();
        return 0;
    }

    int appendFloat64(double value) {
        storage->float_values.push_back(value);
        appendValid();
        return 0;
    }

    int appendDecimal128(const char* value, ExceptionSink* xsink) {
        QoreBufferDecimal128 decimal;
        if (sybase_columnar_parse_decimal128(value, precision, scale, name.c_str(), decimal, xsink)) {
            return -1;
        }
        storage->decimal_values.push_back(decimal);
        appendValid();
        return 0;
    }

    int appendList(QoreValue value, ExceptionSink* xsink) {
        assert(kind == SybaseColumnarKind::List);
        ValueHolder holder(value, xsink);
        if (*xsink) {
            return -1;
        }
        list->push(holder.release(), xsink);
        if (*xsink) {
            return -1;
        }
        ++row_count;
        return 0;
    }

    QoreValue finish(ExceptionSink* xsink) {
        if (kind == SybaseColumnarKind::List) {
            return list.release();
        }

        QoreBufferElementType element_type = kind == SybaseColumnarKind::Float64
            ? QoreBufferElementType::Float64
            : (kind == SybaseColumnarKind::Decimal128 ? QoreBufferElementType::Decimal128
                : QoreBufferElementType::Int64);
        const void* data;
        switch (element_type) {
            case QoreBufferElementType::Float64:
                data = storage->float_values.empty() ? nullptr : storage->float_values.data();
                break;
            case QoreBufferElementType::Decimal128:
                data = storage->decimal_values.empty() ? nullptr : storage->decimal_values.data();
                break;
            default:
                data = storage->int_values.empty() ? nullptr : storage->int_values.data();
                break;
        }

        bool nullable = null_count > 0;
        const uint8_t* validity = nullable && !storage->validity.empty() ? storage->validity.data() : nullptr;
        if (element_type == QoreBufferElementType::Decimal128) {
            return QoreBufferNode::wrapExternalStorage(element_type, nullable, row_count, data, validity, storage,
                null_count, precision, scale, xsink);
        }
        return QoreBufferNode::wrapExternalStorage(element_type, nullable, row_count, data, validity, storage,
            null_count, xsink);
    }

private:
    void ensureValidity() {
        if (!storage->validity.empty()) {
            storage->validity.resize(sybase_columnar_bitmap_size(row_count + 1), 0);
            return;
        }

        storage->validity.resize(sybase_columnar_bitmap_size(row_count + 1), 0xff);
    }

    void appendValid() {
        if (!storage->validity.empty()) {
            sybase_columnar_set_validity_bit(storage->validity, row_count, true);
        }
        ++row_count;
    }

    std::string name;
    SybaseColumnarKind kind = SybaseColumnarKind::List;
    std::shared_ptr<SybaseColumnarStorage> storage;
    ReferenceHolder<QoreListNode> list;
    size_t row_count = 0;
    int64_t null_count = 0;
    int32_t precision = 0;
    int32_t scale = 0;
};
}
#endif

static std::string get_placeholder_at(const Placeholders *ph, size_t i) {
   if (!ph || ph->size() <= i) return ss::string_cast(i);
   if (ph->at(i).empty()) return ss::string_cast(i);
   return ph->at(i);
}

command::command(connection& conn, ExceptionSink* xsink) : m_conn(conn), m_cmd(0), rowcount(-1), lastRes(RES_NONE) {
   CS_RETCODE err = ct_cmd_alloc(m_conn.getConnection(), &m_cmd);
   if (err != CS_SUCCEED) {
      xsink->raiseException("TDS-EXEC-EXCEPTION", "Sybase call ct_cmd_alloc() failed with error %d", (int)err);
      return;
   }
}

//------------------------------------------------------------------------------
command::~command() {
   clear();
}

void command::clear() {
   if (!m_cmd) return;
   // cancel only unfinished, not already canceled command
   if (lastRes != RES_CANCELED && lastRes != RES_END && !m_conn.wasConnectionAborted()) {
      if (ct_cancel(0, m_cmd, CS_CANCEL_ALL) != CS_SUCCEED) {
         throw ss::Error("TDS-EXEC-EXCEPTION", "ct_cancel failed");
      }
   }
   ct_cmd_drop(m_cmd);
   m_cmd = 0;
}

void command::send(ExceptionSink *xsink) {
   // Check for interrupt before sending command
   if (qore_check_cancel(xsink)) {
      return;
   }

   CS_RETCODE err;
   {
      // Register cancel callback for interruptible execution
      QoreSybaseCancelHelper cancel_helper(m_conn.getConnection());
      err = ct_send(m_cmd);
   }

   if (err != CS_SUCCEED) {
      m_conn.do_exception(xsink, "TDS-EXEC-ERROR", "ct_send() failed");
   }
}

void command::initiate_language_command(const char* cmd_text, ExceptionSink* xsink) {
   assert(cmd_text && cmd_text[0]);
   CS_RETCODE err = ct_command(m_cmd, CS_LANG_CMD, (CS_CHAR*)cmd_text, CS_NULLTERM, CS_UNUSED);
   if (err != CS_SUCCEED) {
      m_conn.do_exception(xsink, "TDS-EXEC-ERROR", "ct_command(CS_LANG_CMD, '%s') failed with error %d", cmd_text, (int)err);
   }
}

bool command::fetch_row_into_buffers(ExceptionSink* xsink) {
   CS_INT rows_read;
   CS_RETCODE err;
   {
      // Register cancel callback for interruptible fetch
      QoreSybaseCancelHelper cancel_helper(m_conn.getConnection());
      err = ct_fetch(m_cmd, CS_UNUSED, CS_UNUSED, CS_UNUSED, &rows_read);
   }
    //printd(5, "command::fetch_row_into_buffers() err: %d (CS_END_DATA: %d)\n", err, CS_END_DATA);
    if (err == CS_SUCCEED) {
        if (rows_read != 1) {
            m_conn.do_exception(xsink, "TDS-EXEC-ERROR", "ct_fetch() returned %d rows (expected 1)", (int)rows_read);
        }
        return true;
    }
    if (err == CS_END_DATA) {
        // all data read, we can continue reading results
        lastRes = RES_NONE;
        return false;
    }
    if (err == CS_ROW_FAIL) {
        m_conn.do_exception(xsink, "TDS-EXEC-EXCEPTION", "ct_fetch() returned CS_ROW_FAIL; %d rows read", (int)rows_read);
    } else {
        m_conn.do_exception(xsink, "TDS-EXEC-EXCEPTION", "ct_fetch() returned errno %d", (int)err);
    }
    return false;
}

unsigned command::get_column_count(ExceptionSink* xsink) {
   CS_INT num_cols;
   CS_RETCODE err = ct_res_info(m_cmd, CS_NUMDATA, &num_cols, CS_UNUSED, NULL);
   if (err != CS_SUCCEED) {
      m_conn.do_exception(xsink, "TDS-EXEC-EXCEPTION", "ct_res_info() failed with error %d", (int)err);
   }
   if (num_cols <= 0) {
      m_conn.do_exception(xsink, "TDS-EXEC-EXCEPTION", "ct_res_info() failed");
   }
   return num_cols;
}

// FIXME: use ct_setparam to avoid copying data
void command::set_params(sybase_query &query, const QoreListNode* args, ExceptionSink *xsink) {
    unsigned nparams = query.param_list.size();

    for (unsigned i = 0; i < nparams; ++i) {
        if (query.param_list[i] == 'd')
            continue;

        // The bind arguments arrive as elements of the caller's list, and a member assigned
        // with the weak reference operator ":=" or the opaque reference operator "@=" is
        // stored as the reference rather than its target, so reading the list yields that.
        // Resolve it before dispatching on the type below.
        QoreValue val = args ? args->retrieveEntry(i).resolveIndirect() : QoreValue();

        CS_DATAFMT datafmt;
        memset(&datafmt, 0, sizeof(datafmt));
        datafmt.status = CS_INPUTVALUE;
        datafmt.namelen = CS_NULLTERM;
        sprintf(datafmt.name, "@par%d", int(i + 1));
        datafmt.maxlength = CS_UNUSED;
        datafmt.count = 1;

        CS_RETCODE err = CS_FAIL;

        if (val.isNullOrNothing()) {
#ifdef FREETDS
            // it seems to be necessary to specify a type like
            // this to get a null value to be bound with freetds
            datafmt.datatype = CS_CHAR_TYPE;
            datafmt.format = CS_FMT_NULLTERM;
            datafmt.maxlength = 1;
#endif
            // SQL NULL value
            err = ct_param(m_cmd, &datafmt, 0, CS_UNUSED, -1);
            if (err != CS_SUCCEED) {
                m_conn.do_exception(xsink, "TDS-EXEC-ERROR",
                    "ct_param() for 'null' failed for parameter %u with error %d", i, (int)err);
                return;
            }
            continue;
        }

        qore_type_t ntype = val.getType();

        switch (ntype) {
            case NT_STRING: {
                QoreStringValueHelper str(val);
                // ensure we bind with the proper encoding for the connection
                TempEncodingHelper s(*str, m_conn.getEncoding(), xsink);
                if (!s) throw ss::Error("TDS-EXEC-ERROR", "encoding");

                int slen = s->strlen();
                // note that freetds requires maxlength to be set to the byte length of the string
                // even if CS_FMT_NULLTERM is used, therefore we set CS_FMT_UNUSED
                datafmt.datatype = CS_CHAR_TYPE;
                datafmt.format = CS_FMT_UNUSED;
                // NOTE: setting large sizes here like 2GB works for sybase ctlib,
                // not for freetds
                datafmt.maxlength = slen;
                err = ct_param(m_cmd, &datafmt, (CS_VOID*)s->getBuffer(), slen, 0);
                break;
            }

            case NT_NUMBER: {
                QoreStringValueHelper vh(val);
                int slen = vh->strlen();
                datafmt.datatype = CS_CHAR_TYPE;
                datafmt.format = CS_FMT_NULLTERM;
                datafmt.maxlength = slen + 1;
                err = ct_param(m_cmd, &datafmt, (CS_VOID *)vh->getBuffer(), slen, 0);
                break;
            }

            case NT_DATE: {
                const DateTimeNode* date = val.get<const DateTimeNode>();
                if (m_conn.optimizedDateBinds()) {
                    // must bind the date in the server's time zone
                    qore_tm info;
                    date->getInfo(m_conn.getTZ(), info);
                    QoreStringMaker str("%04d-%02d-%02dT%02d:%02d:%02d", info.year, info.month, info.day, info.hour,
                        info.minute, info.second);
                    if (info.us) {
                        str.sprintf(".%06d", info.us);
                    }
                    datafmt.datatype = CS_CHAR_TYPE;
                    datafmt.format = CS_FMT_UNUSED;
                    int slen = str.strlen();
                    datafmt.maxlength = slen;
                    err = ct_param(m_cmd, &datafmt, (CS_VOID*)str.c_str(), slen, 0);
                } else {
                    // this is the only reliable way to bind values with sub-second resolution
                    CS_DATETIME dt;
                    ss::Conversions conv;
                    if (conv.DateTime_to_DATETIME(date, dt, xsink)) {
                        throw ss::Error("TDS-EXEC-ERROR", "can't convert date");
                    }

                    datafmt.datatype = CS_DATETIME_TYPE;
                    err = ct_param(m_cmd, &datafmt, &dt, sizeof(dt), 0);
                }
                break;
            }

            case NT_INT: {
                int64 ival = val.getAsBigInt();
#ifdef CS_BIGINT_TYPE
                datafmt.datatype = CS_BIGINT_TYPE;
                err = ct_param(m_cmd, &datafmt, &ival, sizeof(int64), 0);
#else
                // if it's a 32-bit integer, bind as integer
                if (ival <= 2147483647 && ival >= -2147483647) {
                    datafmt.datatype = CS_INT_TYPE;
                    CS_INT vint = ival;
                    err = ct_param(m_cmd, &datafmt, &vint, sizeof(CS_INT), 0);
                } else { // bind as float
                    CS_FLOAT fval = ival;
                    datafmt.datatype = CS_FLOAT_TYPE;
                    err = ct_param(m_cmd, &datafmt, &fval, sizeof(CS_FLOAT), 0);
                }
#endif
                break;
            }

            case NT_BOOLEAN: {
                // Seems mssql doesn't like  CS_BIT_TYPE for some reason.
                // Replacing by CS_INT_TYPE helps
                //
                // The "BIT" code is supposed to be like this:
                // datafmt.datatype = CS_BIT_TYPE;
                // err = ct_param(m_cmd, &datafmt, &bval, sizeof(bval), 0);
                // ... but it doesn't work

                CS_BIT bval = val.getAsBool();
                datafmt.datatype = CS_INT_TYPE;
                int64 ival = bval ? 1 : 0;
                err = ct_param(m_cmd, &datafmt, &ival, sizeof(ival), 0);
                break;
            }

            case NT_FLOAT: {
                CS_FLOAT fval = val.getAsFloat();
                datafmt.datatype = CS_FLOAT_TYPE;
                err = ct_param(m_cmd, &datafmt, &fval, sizeof(CS_FLOAT), 0);
                break;
            }

            case NT_BINARY: {
                const BinaryNode *b = val.get<const BinaryNode>();
                datafmt.datatype = CS_BINARY_TYPE;
                datafmt.maxlength = b->size();
                datafmt.count = 1;
                err = ct_param(m_cmd, &datafmt, (void *)b->getPtr(), b->size(), 0);
                break;
            }

            case NT_HASH: {
                const QoreHashNode* h = val.get<const QoreHashNode>();
                QoreValue t = h->getKeyValue("type");
                QoreValue v = h->getKeyValue("value");
                if (t && t.getType() == NT_STRING) {
                    QoreStringValueHelper str(t);
                    if (!strcmp(str->c_str(), "date")) {
                        if (v.getType() != NT_DATE) {
                            m_conn.do_exception(xsink, "TDS-BIND-ERROR", "expecting type 'date' for bind type '%s'; "
                                "got type '%s' instead", str->c_str(), v.getFullTypeName());
                            return;
                        }
                        // NOTE: cannot bind by CS_BIGDATETIME_TYPE
                        // binding by CS_CHAR_TYPE works for BIGDATETIME / DATETIME2 columns, but will fail with a
                        // DATETIME column
                        const DateTimeNode* date = v.get<const DateTimeNode>();
                        qore_tm info;
                        date->getInfo(m_conn.getTZ(), info);
                        QoreStringMaker str("%04d-%02d-%02dT%02d:%02d:%02d", info.year, info.month, info.day,
                            info.hour, info.minute, info.second);
                        if (info.us) {
                            str.sprintf(".%06d", info.us);
                        }
                        datafmt.datatype = CS_CHAR_TYPE;
                        datafmt.format = CS_FMT_UNUSED;
                        int slen = str.strlen();
                        datafmt.maxlength = slen;
                        err = ct_param(m_cmd, &datafmt, (CS_VOID*)str.c_str(), slen, 0);
                        return;
                    }
                    m_conn.do_exception(xsink, "TDS-BIND-ERROR", "unknown explicit bind type '%s'", str->c_str());
                    return;
                }
            }

            default:
                m_conn.do_exception(xsink, "TDS-BIND-ERROR",
                                    "do not know how to bind values of type '%s'",
                                    val.getTypeName());
                return;
        } // switch(ntype)

        if (err != CS_SUCCEED) {
            m_conn.do_exception(xsink, "TDS-EXEC-ERROR",
                                "ct_param() for parameter %u failed with error %d",
                                i, (int)err);
        }
    }
}

int command::get_row_count() {
    return rowcount;
}

command::ResType command::read_next_result1(bool& disconnect, ExceptionSink* xsink) {
    if (*xsink)
        return RES_ERROR;

    switch (lastRes) {
        case RES_DONE:
        case RES_NONE:
        case RES_RETRY:
            // we can read result only in this states
            break;
        default:
            return lastRes;
    }

    // check for interrupt before waiting for results
    if (qore_check_cancel(xsink)) {
        lastRes = RES_CANCELED;
        if (cancelIntern()) {
            disconnect = true;
        }
        return RES_ERROR;
    }

    CS_INT result_type;
    CS_RETCODE err;
    {
        // Register cancel callback for interruptible results fetch
        QoreSybaseCancelHelper cancel_helper(m_conn.getConnection());
        err = ct_results(m_cmd, &result_type);
    }
    //printf("command::read_next_result1 result: %d\n", err);
    switch (err) {
        case CS_END_RESULTS:
            return RES_END;
        case CS_FAIL: {
            if (cancelIntern())
                disconnect = true;
            // TODO: handle err == CS_FAIL
            xsink->raiseException("TDS-EXEC-ERROR",
                                "command::read_output(): ct_results() failed with"
                                " CS_FAIL, command canceled");
            return RES_ERROR;
        }
        case CS_SUCCEED:
            break;
        default:
            m_conn.do_exception(xsink, "TDS-EXEC-ERROR",
                                "command::read_output(): ct_results() returned error code %d", err);
            return RES_ERROR;
    }

    switch (result_type) {
        case CS_CMD_DONE: {
            CS_RETCODE ret;
            rowcount = -1;

            /* from the Sybase docs:
                if ct_results() returns CS_FAIL:
                The routine failed; any remaining results are no longer available.

                If ct_results returns CS_FAIL, an application must call ct_cancel
                with type as CS_CANCEL_ALL before using the affected command
                structure to send another command.

                If ct_cancel returns CS_FAIL, the application must call
                ct_close(CS_FORCE_CLOSE) to force the connection closed.
            */

            ret = ct_res_info(m_cmd, CS_ROW_COUNT,
                            (CS_VOID *)&rowcount,
                            CS_UNUSED, 0);
            if (ret != CS_SUCCEED) {
                m_conn.do_exception(xsink, "TDS-EXEC-EXCEPTION",
                                    "ct_res_info() failed with error %d", (int)ret);
                m_conn.purge_messages(xsink);
                return RES_ERROR;
            }

            colinfo.set_dirty();
            return RES_DONE;
        }
        case CS_CMD_SUCCEED:
            // the command has no output, continue by reading
            // next result
            return RES_RETRY;
        case CS_CMD_FAIL:
            m_conn.do_exception(xsink, "TDS-EXEC-ERROR",
                                "command::read_output(): SQL command failed");
            return RES_ERROR;

        case CS_PARAM_RESULT:
            return RES_PARAM;
        case CS_STATUS_RESULT:
            return RES_STATUS;
        case CS_ROW_RESULT:
            return RES_ROW;
    }

    m_conn.do_exception(xsink, "TDS-EXEC-ERROR", "command::read_output(): ct_results() returned unexpected result type %d", (int)result_type);
    return RES_ERROR;
}

QoreValue command::readOutput(connection& conn, command& cmd, bool list, bool& connection_reset, bool cols, ExceptionSink* xsink, bool single_row) {
    ValueHolder qresult(xsink);

    ss::ResultFactory rf(xsink);

    while (true) {
        ResType rt = conn.readNextResult(cmd, connection_reset, xsink);
        if (*xsink || connection_reset)
            return QoreValue();

        switch (rt) {
            case RES_ERROR:
                return QoreValue();

            case RES_PARAM:
                if (retr_colinfo(xsink))
                    return QoreValue();
                qresult = read_rows(&query->placeholders, xsink);
                //add_rowcount(*qresult, 1, xsink);
                rf.add_params(qresult);
                break;

            case RES_ROW:
                qresult = read_rows(0, list, cols, xsink, single_row);
                rf.add(qresult, list);
                break;

            case RES_END:
                return rf.res();

            case RES_DONE:
                rf.done(rowcount);
                continue;

            case RES_STATUS:
                if (retr_colinfo(xsink))
                    return QoreValue();

                qresult = read_rows(0, list, false, xsink);
                // TODO: check status?
                colinfo.set_dirty();
                continue;

            default:
                m_conn.do_exception(xsink, "TDS-EXEC-ERROR", "command::readOutput(): ct_results() returned unknown result value %d", rt);
                break;
        }
        if (*xsink)
            return QoreValue();
    }
}

int command::retr_colinfo(ExceptionSink* xsink) {
    unsigned columns = get_column_count(xsink);
    if (*xsink)
        return -1;

    colinfo.reset();
    get_row_description(colinfo.datafmt, columns, xsink);
    setup_output_buffers(colinfo.datafmt, xsink);
    colinfo.dirty = false;

    return 0;
}

void command::setupColumns(QoreHashNode& h, const Placeholders *ph) {
    row_result_t &descriptions = colinfo.datafmt;

    for (unsigned i = 0, n = descriptions.size(); i != n; ++i) {
        std::string col_name;

        if (!ss::is_empty(descriptions[i].name)) {
            col_name = descriptions[i].name;
            std::transform(col_name.begin(), col_name.end(), col_name.begin(), ::tolower);
        } else {
            col_name = get_placeholder_at(ph, i);
        }

        HashAssignmentHelper hah(h, col_name);
        if (*hah) {
            // find a unique column name
            unsigned num = 1;
            while (true) {
                QoreStringMaker tmp("%s_%d", col_name.c_str(), num);
                hah.reassign(tmp.c_str());
                if (*hah) {
                ++num;
                continue;
                }
                break;
            }
        }
        hah.assign(new QoreListNode, 0);
    }
}

QoreHashNode* command::read_cols(const Placeholders* ph, int cnt, bool cols, ExceptionSink* xsink) {
    if (ensure_colinfo(xsink)) {
        return nullptr;
    }

    if (xsink->isException()) {
        return nullptr;
    }

    row_result_t &descriptions = colinfo.datafmt;

    // setup hash of lists if necessary
    ReferenceHolder<QoreHashNode> h(new QoreHashNode, xsink);

    if (cols) {
        setupColumns(**h, ph);
    }

    int row_count = 0;
    while (fetch_row_into_buffers(xsink)) {
        // Check for interrupt periodically during fetch (every 100 rows)
        if ((row_count % 100) == 0 && qore_check_cancel(xsink)) {
            return nullptr;
        }
        ++row_count;

        if (h->empty()) {
            setupColumns(**h, ph);
        }
        if (append_buffers_to_list(descriptions, out_buffers, *h, xsink)) {
            return nullptr;
        }
        if (--cnt == 0) {
            break;
        }
    }
    return h.release();
}

QoreHashNode* command::fetch_row(ExceptionSink* xsink, const Placeholders *ph) {
    if (ensure_colinfo(xsink)) return 0;

    if (!fetch_row_into_buffers(xsink)) return 0;
    QoreHashNode *h = output_buffers_to_hash(ph, xsink);
    return h;
}

QoreValue command::read_rows(const Placeholders *ph, ExceptionSink* xsink, bool single_row) {
    if (ensure_colinfo(xsink)) return QoreValue();

    ReferenceHolder<AbstractQoreNode> rv(xsink);
    QoreListNode *l = nullptr;
    int row_count = 0;
    while (fetch_row_into_buffers(xsink)) {
        // Check for interrupt periodically during fetch (every 100 rows)
        if ((row_count % 100) == 0 && qore_check_cancel(xsink)) {
            return QoreValue();
        }
        ++row_count;

        ReferenceHolder<QoreHashNode> h(output_buffers_to_hash(ph, xsink), xsink);
        if (*xsink) return QoreValue();
        if (rv) {
            if (!l) {
                ReferenceHolder<QoreListNode> lholder(new QoreListNode(autoTypeInfo), xsink);
                l = *lholder;
                l->push(rv.release(), xsink);
                rv = lholder.release();
            }
            if (single_row && l->size() == 1) {
                xsink->raiseException("DBI-SELECT-ROW-ERROR", "SQL passed to selectRow() returned more than 1 row");
                return QoreValue();
            }
            l->push(h.release(), xsink);
        }
        else
            rv = h.release();
    }
    return rv.release();
}

QoreValue command::read_rows(Placeholders *placeholder_list, bool list, bool cols, ExceptionSink* xsink,
        bool single_row) {
    if (ensure_colinfo(xsink)) return QoreValue();

    // setup hash of lists if necessary
    if (!list) {
        return read_cols(placeholder_list, cols, xsink);
    } else {
        return read_rows(placeholder_list, xsink, single_row);
    }
}

// returns 0=OK, -1=error (exception raised)
int command::get_row_description(row_result_t &result, unsigned column_count, ExceptionSink* xsink) {
    for (unsigned i = 0; i < column_count; ++i) {
        CS_DATAFMT_EX datafmt;
        memset(&datafmt, 0, sizeof(datafmt));

        CS_RETCODE err = ct_describe(m_cmd, i + 1, &datafmt);
        if (err != CS_SUCCEED) {
            m_conn.do_exception(xsink, "TDS-EXEC-ERROR", "ct_describe() failed with error %d on column %u / %u",
                (int)err, i + 1, column_count);
            return -1;
        }
        datafmt.count = 1; // fetch just single row per every ct_fetch()
        bool is_multi_byte = m_conn.getEncoding()->isMultiByte();

        printd(5, "command::get_row_description(): name: %s type: %d usertype: %d\n",
            datafmt.name, datafmt.datatype, datafmt.usertype);

        datafmt.origin_datatype = datafmt.datatype;
        switch (datafmt.datatype) {
            // we map DECIMAL types to strings so we have no conversion to do
            case CS_DECIMAL_TYPE:
            case CS_NUMERIC_TYPE:
                datafmt.maxlength = 50;
                datafmt.datatype = CS_CHAR_TYPE;
                datafmt.format = CS_FMT_PADBLANK;
                break;

            case CS_UNICHAR_TYPE:
                datafmt.datatype = CS_TEXT_TYPE;
                datafmt.format = CS_FMT_NULLTERM;
                break;

                // freetds only works with CS_FMT_PADBLANK with CS_CHAR columns it seems
                // however this is also compatible with Sybase's ct-lib
            case CS_CHAR_TYPE:
                datafmt.format = CS_FMT_PADBLANK;
                break;

            case CS_LONGCHAR_TYPE:
            case CS_VARCHAR_TYPE:
            case CS_TEXT_TYPE:
                // if it's a multi-byte encoding, double the buffer size
                if (is_multi_byte)
                datafmt.maxlength *= 2;
                datafmt.format = CS_FMT_NULLTERM;
                break;

#ifdef FREETDS
                // FreeTDS seems to return DECIMAL types as FLOAT for some reason
            case CS_FLOAT_TYPE:
                // can't find a defined USER_TYPE_* for 26
                if (datafmt.usertype == 26) {
                datafmt.maxlength = 50;
                datafmt.datatype = CS_CHAR_TYPE;
                datafmt.format = CS_FMT_NULLTERM;
                break;
                }
#endif

            case CS_MONEY_TYPE:
            case CS_MONEY4_TYPE:
                datafmt.datatype = CS_FLOAT_TYPE;
                [[fallthrough]];

            default:
                datafmt.format = CS_FMT_UNUSED;
                break;
        }

        printd(5, "command::get_row_description(): name=%s type=%d usertype=%d maxlength=%d\n", datafmt.name,
            datafmt.datatype, datafmt.usertype, datafmt.maxlength);

        result.push_back(datafmt);
    }
    return 0;
}

int command::setup_output_buffers(const row_result_t &input_row_descriptions, ExceptionSink *xsink) {
    out_buffers.reset();
    for (unsigned i = 0, n = input_row_descriptions.size(); i != n; ++i) {
        unsigned size = input_row_descriptions[i].maxlength;
        output_value_buffer *out = out_buffers.insert(size);

        CS_RETCODE err = ct_bind(m_cmd, i + 1,
                                (CS_DATAFMT*)&input_row_descriptions[i],
                                out->value, &out->value_len, &out->indicator);

        if (err != CS_SUCCEED) {
            m_conn.do_exception(xsink, "TDS-EXEC-ERROR", "ct_bind() failed with error %d", (int)err);
            return -1;
        }
    }
    return 0;
}

int command::append_buffers_to_list(row_result_t &column_info, row_output_buffers& all_buffers, QoreHashNode *h, ExceptionSink *xsink) {
    HashIterator hi(h);
    for (unsigned i = 0, n = column_info.size(); i != n; ++i) {
        hi.next();

        const output_value_buffer& buff = *all_buffers[i];
        QoreValue value = get_value(column_info[i], buff, xsink);
        if (xsink->isException()) {
            value.discard(xsink);
            return -1;
        }

        QoreListNode* l = hi.get().get<QoreListNode>();
        l->push(value, xsink);
    } // for

    return 0;
}

QoreHashNode *command::output_buffers_to_hash(const Placeholders *ph, ExceptionSink* xsink) {
    row_result_t &column_info = colinfo.datafmt;
    ReferenceHolder<QoreHashNode> result(new QoreHashNode, xsink);

    for (unsigned i = 0, n = column_info.size(); i != n; ++i) {
        const output_value_buffer& buff = *out_buffers[i];

        ValueHolder value(get_value(column_info[i], buff, xsink), xsink);

        if (*xsink) return 0;

        std::string column_name;
        if (!ss::is_empty(column_info[i].name)) {
            column_name = column_info[i].name;
            std::transform(column_name.begin(), column_name.end(), column_name.begin(), ::tolower);
        } else {
            column_name = get_placeholder_at(ph, i);
        }

        HashAssignmentHelper hah(**result, column_name);
        if (*hah) {
            // find a unique column name
            unsigned num = 1;
            while (true) {
                QoreStringMaker tmp("%s_%d", column_name.c_str(), num);
                hah.reassign(tmp.c_str());
                if (*hah) {
                ++num;
                continue;
                }
                break;
            }
        }

        hah.assign(value.release(), xsink);
    }

    return result.release();
}

static bool is_number(const CS_DATAFMT_EX& datafmt) {
    switch (datafmt.origin_datatype) {
        case CS_DECIMAL_TYPE:
        case CS_NUMERIC_TYPE:
            return true;
    }
    return false;
}

static inline bool need_trim(const CS_DATAFMT_EX& datafmt) {
    if (datafmt.format == CS_FMT_PADBLANK || datafmt.usertype == 34 ||
        // seems TEXT needs trim as well (found on mssql, sybase-test.q)
        datafmt.datatype == CS_TEXT_TYPE
#ifdef SYBASE
        // for some reason sybase returns a char field as LONGCHAR when the
        // server is uses iso_1 character encoding, but the connection is set
        // to utf-8 also in this case the result is always blank padded even
        // though the datafmt.format is set to CS_FMT_NULLTERM
        || datafmt.datatype == CS_LONGCHAR_TYPE
#endif
        ) {
        return true;
    }
    return false;
}

QoreValue command::getNumber(const char* str, size_t len) {
    assert(!str[len]);
    int nf = m_conn.getNumeric();

    assert(nf != connection::OPT_NUM_STRING);

    // trim off trailing zeros after the decimal in any case
    bool has_decimal = (bool)strchr(str, '.');
    if (has_decimal) {
        char* c = (char*)str;
        // trim off trailing zeros
        while (len && c[len - 1] == '0') {
            --len;
            c[len] = '\0';
        }
        if (c[len - 1] == '.') {
            --len;
            c[len] = '\0';
            has_decimal = false;
        }
    }
    //printf("num: '%s' has_dec: %d\n", str, has_decimal);

    if (nf == connection::OPT_NUM_OPTIMAL && !has_decimal) {
        bool sign = str[0] == '-';
        if (sign)
            --len;
        if (!strchr(str, '.')
            && (len < 19
                || (len == 19 &&
                    ((!sign && strcmp(str, "9223372036854775807") <= 0)
                    ||(sign && strcmp(str, "-9223372036854775808") >= 0)))))
            return strtoll(str, 0, 10);
    }

    return new QoreNumberNode(str);
}

QoreValue command::get_value(const CS_DATAFMT_EX& datafmt, const output_value_buffer& buffer, ExceptionSink* xsink) {
    if (buffer.indicator == -1) { // SQL NULL
        return null();
    }

    const QoreEncoding* encoding = m_conn.getEncoding();

    switch (datafmt.datatype) {
        case CS_LONGCHAR_TYPE:
        case CS_VARCHAR_TYPE:
        case CS_TEXT_TYPE: {
            CS_CHAR* value = (CS_CHAR*)(buffer.value);

            // copy the value to a null-terminated string for processing
            QoreString tmp((const char*)value, buffer.value_len - 1);

#ifdef SYBASE
            if (need_trim(datafmt))
                tmp.trim_trailing(' ');
#endif

            if (is_number(datafmt) && m_conn.getNumeric() != connection::OPT_NUM_STRING)
                return getNumber(tmp.c_str(), tmp.size());

            size_t len = tmp.size();
            size_t all = tmp.capacity();
            return new QoreStringNode(tmp.giveBuffer(), len, all, encoding);
        }

        case CS_CHAR_TYPE: {
            CS_CHAR* value = (CS_CHAR*)(buffer.value);

            // copy the value to a null-terminated string for processing
            QoreString tmp((const char*)value, buffer.value_len);
            if (need_trim(datafmt))
                tmp.trim_trailing(' ');

            if (is_number(datafmt) && m_conn.getNumeric() != connection::OPT_NUM_STRING)
                return getNumber(tmp.c_str(), tmp.size());

            size_t len = tmp.size();
            size_t all = tmp.capacity();
            return new QoreStringNode(tmp.giveBuffer(), len, all, encoding);
        }

        case CS_VARBINARY_TYPE:
        case CS_BINARY_TYPE:
        case CS_LONGBINARY_TYPE:
        case CS_IMAGE_TYPE: {
            CS_BINARY* value = (CS_BINARY*)(buffer.value);
            int size = buffer.value_len;
            void* block = malloc(size);
            if (!block) {
                xsink->outOfMemory();
                return 0;
            }
            memcpy(block, value, size);
            return new BinaryNode(block, size);
        }

        case CS_TINYINT_TYPE: {
            CS_TINYINT* value = (CS_TINYINT*)(buffer.value);
            return (int64)*value;
        }

        case CS_SMALLINT_TYPE: {
            CS_SMALLINT* value = (CS_SMALLINT*)(buffer.value);
            return (int64)*value;
        }

        case CS_INT_TYPE: {
            CS_INT* value = (CS_INT*)(buffer.value);
            return (int64)*value;
        }

#ifdef CS_BIGINT_TYPE
        case CS_BIGINT_TYPE: {
            int64 *value = (int64 *)(buffer.value);
            return (int64)*value;
        }
#endif

        case CS_REAL_TYPE: {
            CS_REAL* value = (CS_REAL*)(buffer.value);
            return (double)*value;
        }

        case CS_FLOAT_TYPE: {
            CS_FLOAT* value = (CS_FLOAT*)(buffer.value);
            return (double)*value;
        }

        case CS_BIT_TYPE: {
            CS_BIT* value = (CS_BIT*)(buffer.value);
            return (*value != 0);
        }

        case CS_DATETIME_TYPE: {
            CS_DATETIME* value = (CS_DATETIME*)(buffer.value);

            ss::Conversions conv;
            // NOTE: can't find a USER_* define for 38!
            if (datafmt.usertype == 38)
                return conv.TIME_to_DateTime(*value, m_conn.getTZ());

            return conv.DATETIME_to_DateTime(*value, m_conn.getTZ());
        }

        case CS_DATETIME4_TYPE: {
            ss::Conversions conv;
            CS_DATETIME4* value = (CS_DATETIME4*)(buffer.value);
            return conv.DATETIME4_to_DateTime(*value);
        }

#ifdef CS_BIGDATETIME_TYPE
        case CS_BIGDATETIME_TYPE: {
            // number of microseconds after 0000-01-01
            uint64_t* value = (uint64_t*)(buffer.value);
            return ss::Conversions::BIGDATETIME_to_DateTime(*value, m_conn.getTZ());
        }
#endif

#ifdef CS_BIGTIME_TYPE
        case CS_BIGTIME_TYPE: {
            // number of microseconds after the beginning of the day
            uint64_t* value = (uint64_t*)(buffer.value);
            return ss::Conversions::BIGTIME_to_DateTime(*value, m_conn.getTZ());
        }
#endif

#ifdef CS_DATE_TYPE
        case CS_DATE_TYPE: {
            // number of days since 1900-01-01
            unsigned* value = (unsigned*)(buffer.value);
            return ss::Conversions::DATE_to_DateTime(*value, m_conn.getTZ());

        }
#endif

        default:
            xsink->raiseException("TDS-EXEC-EXCEPTION", "Unknown data type %d", (int)datafmt.datatype);
            return QoreValue();
    } // switch
}

#ifdef HAVE_QORE_COLUMNAR_RESULT
QoreColumnarResult* command::read_columnar(const Placeholders* ph, int cnt, ExceptionSink* xsink) {
    if (ensure_colinfo(xsink)) {
        return nullptr;
    }

    row_result_t& descriptions = colinfo.datafmt;
    std::vector<std::unique_ptr<SybaseColumnarBuilder>> builders;
    builders.reserve(descriptions.size());
    for (size_t i = 0; i < descriptions.size(); ++i) {
        if (i && !(i % 100) && qore_check_cancel(xsink, "initializing Sybase columnar result")) {
            return nullptr;
        }

        std::string col_name;
        if (!ss::is_empty(descriptions[i].name)) {
            col_name = descriptions[i].name;
            std::transform(col_name.begin(), col_name.end(), col_name.begin(), ::tolower);
        } else {
            col_name = get_placeholder_at(ph, i);
        }
        builders.emplace_back(new SybaseColumnarBuilder(descriptions[i], col_name.c_str(), m_conn.getNumeric(), xsink));
    }

    int row_count = 0;
    while (fetch_row_into_buffers(xsink)) {
        if ((row_count % 100) == 0 && qore_check_cancel(xsink)) {
            return nullptr;
        }

        for (size_t i = 0; i < descriptions.size(); ++i) {
            if (i && !(i % 100) && qore_check_cancel(xsink, "fetching Sybase columnar row")) {
                return nullptr;
            }

            const output_value_buffer& buff = *out_buffers[i];
            SybaseColumnarBuilder& builder = *builders[i];
            if (builder.getKind() == SybaseColumnarKind::List) {
                if (builder.appendList(get_value(descriptions[i], buff, xsink), xsink)) {
                    return nullptr;
                }
                continue;
            }

            if (buff.indicator == -1) {
                builder.appendNull();
                continue;
            }

            switch (descriptions[i].datatype) {
                case CS_TINYINT_TYPE:
                    builder.appendInt64(static_cast<int64>(*reinterpret_cast<CS_TINYINT*>(buff.value)));
                    break;

                case CS_SMALLINT_TYPE:
                    builder.appendInt64(static_cast<int64>(*reinterpret_cast<CS_SMALLINT*>(buff.value)));
                    break;

                case CS_INT_TYPE:
                    builder.appendInt64(static_cast<int64>(*reinterpret_cast<CS_INT*>(buff.value)));
                    break;

#ifdef CS_BIGINT_TYPE
                case CS_BIGINT_TYPE:
                    builder.appendInt64(static_cast<int64>(*reinterpret_cast<int64*>(buff.value)));
                    break;
#endif

                case CS_REAL_TYPE:
                    builder.appendFloat64(static_cast<double>(*reinterpret_cast<CS_REAL*>(buff.value)));
                    break;

                case CS_FLOAT_TYPE:
                    builder.appendFloat64(static_cast<double>(*reinterpret_cast<CS_FLOAT*>(buff.value)));
                    break;

                case CS_CHAR_TYPE: {
                    QoreString tmp(reinterpret_cast<const char*>(buff.value), buff.value_len);
                    if (need_trim(descriptions[i])) {
                        tmp.trim_trailing(' ');
                    }
                    if (builder.getKind() == SybaseColumnarKind::Int64) {
                        int64 value;
                        if (!sybase_columnar_parse_int64(tmp.c_str(), value)) {
                            xsink->raiseException("SYBASE-COLUMNAR-DECIMAL-ERROR",
                                "cannot convert DECIMAL column '%s' value '%s' to int64 without losing precision",
                                builder.getName(), tmp.c_str());
                            return nullptr;
                        }
                        builder.appendInt64(value);
                    } else if (builder.appendDecimal128(tmp.c_str(), xsink)) {
                        return nullptr;
                    }
                    break;
                }

                default:
                    assert(false);
                    break;
            }
        }

        ++row_count;
        if (--cnt == 0) {
            break;
        }
    }

    ReferenceHolder<QoreHashNode> columns(new QoreHashNode, xsink);
    for (size_t i = 0; i < builders.size(); ++i) {
        if (i && !(i % 100) && qore_check_cancel(xsink, "finalizing Sybase columnar result")) {
            return nullptr;
        }

        HashAssignmentHelper hah(**columns, builders[i]->getName());
        if (*hah) {
            unsigned num = 1;
            while (true) {
                if (!(num % 100) && qore_check_cancel(xsink, "deduplicating Sybase columnar column names")) {
                    return nullptr;
                }

                QoreStringMaker tmp("%s_%d", builders[i]->getName(), num);
                hah.reassign(tmp.c_str());
                if (*hah) {
                    ++num;
                    continue;
                }
                break;
            }
        }
        hah.assign(builders[i]->finish(xsink), xsink);
        if (*xsink) {
            return nullptr;
        }
    }

    return QoreColumnarResult::fromColumnHash(*columns, nullptr, xsink);
}
#endif

int command::bind_query(std::unique_ptr<sybase_query>& q, const QoreListNode* args, ExceptionSink* xsink) {
    query.reset(q.release());

    initiate_language_command(query->buff(), xsink);

    if (args) set_params(*query, args, xsink);

    return 0;
}
