RPM packaging
=============

Copyright 2026 Qore Technologies, s.r.o.

The canonical qore-sybase-modules.spec builds qore-freetds-module and its
separate documentation package for Fedora, Enterprise Linux and openSUSE.
The public recipe uses distribution FreeTDS headers and shared libraries.
The proprietary Sybase OCS driver remains a separate SDK and qualification
requirement, as in the Debian packaging. FreeTDS itself supports connections
to both Microsoft SQL Server and Sybase databases.

Prepare sources with the qore-packaging tools::

    python3 tools/packaging.py prepare --repo ../module-sybase --ref COMMIT \
      --name qore-freetds-module --version 1.4 \
      --spec qore-sybase-modules.spec --exclude m4/acx_pthread.m4 \
      --output work/freetds-source
    python3 tools/build-local.py --source work/freetds-source \
      --image TARGET_SDK_IMAGE --output results/freetds-build --jobs 2

The unused legacy Autoconf pthread macro has no license grant and is excluded
from the source bundle, matching the Debian source recipe. CMake does not use
that macro. All native source and license notices are retained.

Builds use the packaged Qore SDK, distribution optimization/hardening flags,
normal RPM debug packages and source-prefix maps. Documentation warnings fail
the build. No server is installed and no FreeTDS configuration is modified.

The default offline check selects exactly one native module, clears environment
overrides and runs all four driver tests (14 assertions): capabilities, invalid
protocol rejection, missing username rejection and unopened-connection cleanup.
Missing modules, ambiguous ABI artifacts, incomplete tests, skips and warnings
fail qualification. The tests do not contact a server.

Run installed-package checks from the corresponding source bundle as a normal
user outside a build tree. The test harness requires Python 3; it is not a
runtime dependency of the driver itself::

    /usr/bin/python3 -B -W error rpm/run-tests.py --installed

With qore-devel installed, also check compiler use and native driver loading::

    /usr/bin/python3 -B -W error rpm/run-tests.py --installed --compiler

The server-backed suites under test/ cover database behavior, including bulk
loading, cancellation, reconnects, stored procedures and large objects. They
require a separately configured disposable database and remain an explicit
integration gate; passing the offline package checks does not claim server
qualification. Native Sybase OCS builds are likewise outside this public
FreeTDS recipe's qualification scope.
