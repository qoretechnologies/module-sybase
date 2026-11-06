#ifndef SYBASE_SRC_ERROR_H
#define SYBASE_SRC_ERROR_H

#include <string>

#include "qore/ExceptionSink.h"

namespace ss {

class Error {
public:
    DLLLOCAL Error(const std::string& e1, const std::string& e2) :
        e1(e1), e2(e2) {

    }

    DLLLOCAL void raise(ExceptionSink* xsink) const {
        if (xsink->isException()) {
            return;
        }
        xsink->raiseException(e1.c_str(), "%s", e2.c_str());
    }

    DLLLOCAL const char* getErr() const {
        return e1.c_str();
    }

    DLLLOCAL const char* getDesc() const {
        return e2.c_str();
    }

private:
    std::string e1;
    std::string e2;
};

}

#endif /* SYBASE_SRC_ERROR_H */
