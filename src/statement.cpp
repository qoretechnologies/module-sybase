/* -*- indent-tabs-mode: nil -*- */

#include "statement.h"

void ss::Statement::Delete(Statement* self, ExceptionSink* xsink) {
   if (!self) {
      return;
   }
   self->set_res(nullptr, xsink);
   // the results that were not read are discarded with their messages, so that they are not raised by the next call
   if (self->valid && self->context.get()) {
      connection& conn = self->context->getConnection();
      self->context.reset();
      conn.discard_messages();
   }
   delete self;
}

void ss::Statement::discardResults(connection& conn) {
   results_done = true;
   if (valid) {
      context->cancel();
      conn.discard_messages();
   }
}

int ss::Statement::affected_rows(SQLStatement* stmt, ExceptionSink* xsink) {
   if (checkValid(xsink))
      return 0;
   // the total of the row counts read so far
   return context->get_affected_rows();
}

int ss::Statement::exec(connection *conn, const QoreString *query, const QoreListNode *args, bool raw, ExceptionSink* xsink) {
   if (checkValid(xsink))
      return -1;
   if (context.get()) {
      // the results of the previous execution that were not read are discarded with their messages
      context->cancel();
      conn->discard_messages();
   }
   results_done = false;

   context.reset(conn->setupCommand(query, args, raw, xsink));
   if (*xsink)
      return -1;

   // the statement returns the rows of its first result set; the row counts and return status results before it are
   // read here, and without a result set, every result is read here
   try {
      if (positionOnResultSet(conn, xsink) || conn->purge_messages(xsink)) {
         discardResults(*conn);
         return -1;
      }
   } catch (const ss::Error&) {
      discardResults(*conn);
      throw;
   }
   return 0;
}

int ss::Statement::positionOnResultSet(connection* conn, ExceptionSink* xsink) {
   bool connection_reset = false;
   while (true) {
      command::ResType res = conn->readNextResult(*context.get(), connection_reset, xsink);
      if (*xsink) {
         return -1;
      }
      assert(!connection_reset);
      switch (res) {
         case command::RES_DONE:
            continue;

         case command::RES_STATUS:
            if (context->skip_result(xsink)) {
               return -1;
            }
            continue;

         case command::RES_ROW:
         case command::RES_PARAM:
            return 0;

         default:
            // all results have been read (RES_END), or the command was canceled
            results_done = true;
            return 0;
      }
   }
}

int ss::Statement::finishResults(SQLStatement* stmt, ExceptionSink* xsink) {
   assert(!results_done);
   // the rows end here, even if a later result raises an error
   results_done = true;
   connection* conn = static_cast<connection*>(stmt->getDatasource()->getPrivateData());
   try {
      // errors reported in messages only, such as RAISERROR in a procedure, are raised here as well
      if (context->finish_results(*conn, "the SQL executed by SQLStatement", xsink) || conn->purge_messages(xsink)) {
         discardResults(*conn);
         return -1;
      }
   } catch (const ss::Error&) {
      discardResults(*conn);
      throw;
   }
   return 0;
}

bool ss::Statement::next(SQLStatement* stmt, ExceptionSink* xsink) {
   if (checkValid(xsink))
      return false;
   // a row read by a previous call and not yet returned
   if (has_res()) {
      return true;
   }
   if (results_done) {
      return false;
   }
   connection* conn = static_cast<connection*>(stmt->getDatasource()->getPrivateData());

   bool connection_reset = false;
   command::ResType res = conn->readNextResult(*context.get(), connection_reset, xsink);
   if (*xsink)
      return false;
   assert(!connection_reset);

   if (!expect_row(res)) {
      // the rows of the result set have been read by another call: the remaining results are read
      finishResults(stmt, xsink);
      return false;
   }

   set_res(context->fetch_row(xsink), xsink);
   if (*xsink) {
      return false;
   }
   if (has_res()) {
      return true;
   }
   // the end of the result set: the remaining results are read, so that the error of a later statement is raised
   finishResults(stmt, xsink);
   return false;
}

void ss::init(qore_dbi_method_list &methods) {
    DBModuleWrap<Statement> module(methods);
    module.reg();
}
