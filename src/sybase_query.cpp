/*
    sybase_query.cpp

    Sybase DB layer for QORE
    uses Sybase OpenClient C library

    Qore Programming language

    Copyright (C) 2007 - 2026 Qore Technologies

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
#include <ctype.h>
#include <string.h>

#include "sybase.h"
#include "sybase_query.h"

// issue #4710: append a T-SQL Unicode (nvarchar) string literal for the given value
// to "out", using the encoding of the SQL text.
//
// Security: the ONLY metacharacter in a T-SQL single-quoted string literal is the
// single quote, escaped by doubling (' -> '').  T-SQL has no backslash escaping and
// single-quoted literals are unaffected by SET QUOTED_IDENTIFIER, so this single,
// total transformation fully neutralizes SQL injection.  The escaper operates on the
// exact byte buffer (size(), not strlen()) so an embedded NUL cannot be used to
// smuggle unescaped content; a value containing a raw NUL (which cannot appear in a
// T-SQL literal at all) is reported so the caller falls back to parameter binding.
//
// returns: 0 = literal appended; 1 = embedded NUL, caller must bind as a parameter;
//          -1 = exception raised
static int append_tsql_nvarchar_literal(QoreString& out, const QoreValue& v,
        const QoreEncoding* enc, ExceptionSink* xsink) {
    QoreStringValueHelper sv(v);
    // emit the literal in the same character encoding as the SQL text
    TempEncodingHelper str(*sv, enc, xsink);
    if (!str) {
        return -1;
    }
    const char* buf = str->c_str();
    size_t len = str->size();
    if (memchr(buf, 0, len)) {
        return 1;
    }
    out.concat("N'");
    for (size_t i = 0; i < len; ++i) {
        char c = buf[i];
        if (c == '\'') {
            out.concat("''");
        } else {
            out.concat(c);
        }
    }
    out.concat('\'');
    return 0;
}

// Skips a delimited token whose closing delimiter is escaped by doubling it, as T-SQL single-quoted strings,
// double-quoted identifiers and strings, and bracketed identifiers are; "s" points after the opening delimiter.
// Returns false if the token is not terminated.
static bool skip_delimited(const char*& s, char close) {
    while (*s) {
        if (*s++ == close) {
            if (*s != close) {
                return true;
            }
            // the delimiter is doubled: it is part of the token
            ++s;
        }
    }
    return false;
}

static bool is_name_char(char c) {
    return isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// returns 0=OK, -1=error (exception raised)
// The SQL text is read as T-SQL: placeholders are not recognized in single-quoted strings, double-quoted strings and
// identifiers, bracketed identifiers ([name]) and comments (-- to the end of the line, and /* */, which nest), a
// "::" scope qualifier (as in "schema::name" or "geometry::Point()") is not a placeholder, nor is a ':' that is not
// followed by a name (as after a label), and a '%' that is not followed by a letter is the modulo operator.
int sybase_query::init(const QoreString *cmd_text,
        const QoreListNode *args,
        bool mssql,
        ExceptionSink *xsink)
{
   m_cmd = *cmd_text;

   const char* s = m_cmd.getBuffer();
   QoreString tmp;
   while (*s) {
       char ch = *s++;

       // skip line comments
       if (ch == '-' && *s == '-') {
           while (*s && *s != '\n' && *s != '\r') {
               ++s;
           }
           continue;
       }
       // skip block comments, which nest in T-SQL
       if (ch == '/' && *s == '*') {
           ++s;
           int depth = 1;
           while (*s && depth) {
               if (*s == '/' && s[1] == '*') {
                   ++depth;
                   s += 2;
               } else if (*s == '*' && s[1] == '/') {
                   --depth;
                   s += 2;
               } else {
                   ++s;
               }
           }
           continue;
       }
       // skip single-quoted strings, double-quoted strings and identifiers, and bracketed identifiers; a doubled
       // closing delimiter is part of the token
       if (ch == '\'' || ch == '"' || ch == '[') {
           if (!skip_delimited(s, ch == '[' ? ']' : ch)) {
               // unterminated string or identifier - return without error; the DB will report it
               return 0;
           }
           continue;
       }

       if (ch == '%') {
           // a '%' that is not followed by a letter is the modulo operator
           if (!isalpha(static_cast<unsigned char>(*s))) {
               continue;
           }
           int offset = s - m_cmd.getBuffer() - 1;
           ch = *s++;
           if (ch == 'v') {
               // issue #4710: with FreeTDS ct-lib there is no parameter type that
               // sends a native NVARCHAR value, so string arguments are converted to
               // the server's single-byte code page and characters outside it are
               // lost/rejected.  For MS SQL Server, inline string values into the SQL
               // text as escaped Unicode N'...' literals instead: the query batch is
               // transmitted as UCS-2 (TDS 7+), so any Unicode value reaches NCHAR/
               // NVARCHAR/NTEXT columns losslessly regardless of the database's
               // default collation, and MS SQL implicitly converts to non-Unicode
               // columns as needed.
               QoreValue pv = args ? args->retrieveEntry(param_list.size()) : QoreValue();
               if (mssql && pv.getType() == NT_STRING) {
                   tmp.clear();
                   tmp.setEncoding(m_cmd.getEncoding());
                   int rc = append_tsql_nvarchar_literal(tmp, pv, m_cmd.getEncoding(), xsink);
                   if (rc < 0) {
                       return -1;
                   }
                   if (rc == 0) {
                       // inlined as a literal; mark 'd' so it is not bound as a param
                       param_list.push_back('d');
                       m_cmd.replace(offset, 2, tmp.c_str());
                       s = m_cmd.getBuffer() + offset + tmp.strlen();
                       goto next;
                   }
                   // rc == 1: embedded NUL - fall through to parameter binding
               }

               param_list.push_back('v');
               //param_list.resize(count + 1);
               //param_list[count++].set(PN_VALUE);

               tmp.clear();
               tmp.sprintf("@par%u", param_list.size());
               m_cmd.replace(offset, 2, tmp.c_str());
               s = m_cmd.getBuffer() + offset + tmp.strlen();
           } else if (ch == 'd') {
               QoreValue v = args ? args->retrieveEntry(param_list.size()) : QoreValue();
               tmp.clear();
               DBI_concat_numeric(&tmp, v);
               m_cmd.replace(offset, 2, tmp.c_str());
               s = m_cmd.getBuffer() + offset + tmp.strlen();

               param_list.push_back('d');
               //param_list.resize(count + 1);
               //param_list[count++].set(PN_NUMERIC);
           } else if (ch == 's') {
               QoreValue v = args ? args->retrieveEntry(param_list.size()) : QoreValue();
               tmp.clear();
               if (DBI_concat_string(&tmp, v, xsink))
                   return -1;
               m_cmd.replace(offset, 2, tmp.c_str());
               s = m_cmd.getBuffer() + offset + tmp.strlen();

               // mark it with a 'd' to ensure it gets skipped
               param_list.push_back('d');
               //param_list.resize(count + 1);
               //param_list[count++].set(PN_NUMERIC);
           } else {
               xsink->raiseException("DBI-EXEC-EXCEPTION",
                       "Only %%v or %%d expected in parameter list");
               return -1;
           }
       } else if (ch == ':') {
           // "::" is a T-SQL scope qualifier, as in "schema::name" or "geometry::Point()", not a placeholder
           if (*s == ':') {
               while (*s == ':') {
                   ++s;
               }
               continue;
           }
           // a ':' that is not followed by a name, such as after a label, is not a placeholder
           if (!is_name_char(*s)) {
               continue;
           }
           // read placeholder name
           int offset = s - m_cmd.getBuffer() - 1;

           const char* placeholder_start = s;
           while (is_name_char(*s)) ++s;
           m_cmd.replace(offset, 1, "@");

           //param_list.resize(count + 1);
           //param_list[count++].set(placeholder_start, s - placeholder_start);

           // is this creating a std::string and then copying it and creating another in the vector?
           //placeholder_list.add(placeholder_start, s - placeholder_start);

           std::string ph(placeholder_start, s - placeholder_start);
           placeholders.push_back(ph);
       }
next:
       ;
   } // while

   //printd(5, "size=%d, m_cmd=%s\n", param_list.size(), m_cmd.getBuffer());
   return 0;
}

