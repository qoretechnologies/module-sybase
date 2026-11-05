#!/bin/bash

set -e
set -x

ENV_FILE=/tmp/env.sh

. ${ENV_FILE}

# setup MODULE_SRC_DIR env var
cwd=`pwd`
if [ -z "${MODULE_SRC_DIR}" ]; then
    if [ -e "$cwd/src/sybase.h" ] || [ -e "$cwd/src/sybase.cpp" ]; then
        MODULE_SRC_DIR=$cwd
    else
        MODULE_SRC_DIR=$WORKDIR/module-sybase
    fi
fi
echo "export MODULE_SRC_DIR=${MODULE_SRC_DIR}" >> ${ENV_FILE}

echo "export QORE_UID=1000" >> ${ENV_FILE}
echo "export QORE_GID=1000" >> ${ENV_FILE}

. ${ENV_FILE}

export MAKE_JOBS=4

# the tests must load the drivers of this tree: reject relative module paths in them, and require the build directory
# in the module path of every test that loads a driver
${MODULE_SRC_DIR}/test/docker_test/check-module-paths.sh freetds sybase

# build module and install
echo && echo "-- building module --"
mkdir -p ${MODULE_SRC_DIR}/build
cd ${MODULE_SRC_DIR}/build
cmake .. -DCMAKE_BUILD_TYPE=debug -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX}
make -j${MAKE_JOBS}
make install

# add Qore user and group
if ! grep -q "^qore:x:${QORE_GID}" /etc/group; then
    addgroup -g ${QORE_GID} qore
fi
if ! grep -q "^qore:x:${QORE_UID}" /etc/passwd; then
    adduser -u ${QORE_UID} -D -G qore -h /home/qore -s /bin/bash qore
fi

# own everything by the qore user
chown -R qore:qore ${MODULE_SRC_DIR}

# run the tests
export QORE_MODULE_DIR=${MODULE_SRC_DIR}/qlib:${MODULE_SRC_DIR}/build:${QORE_MODULE_DIR}
cd ${MODULE_SRC_DIR}
# run every test suite and aggregate failures so that one failing suite does
# not mask the results of the others (the loop runs alphabetically, so a single
# early failure would otherwise abort the whole job via "set -e")
failed=""
# the tests must load the modules that this job built, not the copies that the image already has
${MODULE_SRC_DIR}/test/docker_test/check-loaded-modules.sh ${MODULE_SRC_DIR}/build gosu qore:qore

for test in test/*.qtest; do
    echo "=== running ${test} ==="
    if ! gosu qore:qore qore $test -vv; then
        failed="${failed} ${test}"
    fi
done

# ... and they still do once the tests have run
${MODULE_SRC_DIR}/test/docker_test/check-loaded-modules.sh ${MODULE_SRC_DIR}/build gosu qore:qore
if [ -n "${failed}" ]; then
    echo "FAILED test suites:${failed}"
    exit 1
fi
echo "all test suites passed"
