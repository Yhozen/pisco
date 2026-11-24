# SPDX-License-Identifier: GPL-2.0-or-later
# env.sh: source it. Puts the oracle's Wine on PATH and sets what a headless run needs.
#   ORACLE_HOME   where prefixes, the Python environment and recordings live (default ~/.series-web/oracle)
#   WINE_HOME     the portable Wine (default ~/opt/wine-11.22-amd64-wow64; tools/oracle-wine/setup.sh fetches it)
# The build is Kron4ek's "amd64-wow64": 32-bit programs run without any 32-bit system library.
export ORACLE_HOME=${ORACLE_HOME:-$HOME/.series-web/oracle}
export WINE_VERSION=${WINE_VERSION:-11.22}
export WINE_HOME=${WINE_HOME:-$HOME/opt/wine-$WINE_VERSION-amd64-wow64}
export PATH=$WINE_HOME/bin:$PATH
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLOVERRIDES="mscoree,mshtml="   # no Mono / Gecko: their install prompts hang without a display
export LANG=C.UTF-8 LC_ALL=C.UTF-8          # installers with non-ASCII file names fail in the C locale
