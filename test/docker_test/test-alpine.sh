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

# every file that this job installs is newer than this one
build_start=$(mktemp)

# build module and install
echo && echo "-- building module --"
cd ${MODULE_SRC_DIR}
./reconf.sh
./configure --enable-debug --prefix=${INSTALL_PREFIX}
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
# Include both the source qlib dir and the installed module path
export QORE_MODULE_DIR=${MODULE_SRC_DIR}/qlib:${INSTALL_PREFIX}/lib/qore-modules:${QORE_MODULE_DIR}
cd ${MODULE_SRC_DIR}
# run every test suite and aggregate failures so that one failing suite does
# not mask the results of the others (the loop runs alphabetically, so a single
# early failure would otherwise abort the whole job via "set -e")
failed=""
# the tests must load the module that this job built and installed, not one that the image already has
loaded=$(gosu qore:qore qore -l freetds -e 'printf("%s", get_module_hash().freetds.filename);')
if [ ! "${loaded}" -nt "${build_start}" ]; then
    echo "ERROR: the tests would load '${loaded}', which this job did not install" >&2
    exit 1
fi
echo "-- the tests load ${loaded}"

for test in test/*.qtest; do
    echo "=== running ${test} ==="
    if ! gosu qore:qore qore $test -vv; then
        failed="${failed} ${test}"
    fi
done
if [ -n "${failed}" ]; then
    echo "FAILED test suites:${failed}"
    exit 1
fi
echo "all test suites passed"
