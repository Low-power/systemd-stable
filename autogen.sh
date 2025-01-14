#!/bin/sh

#  This file is part of systemd.
#
#  systemd is free software; you can redistribute it and/or modify it
#  under the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation; either version 2.1 of the License, or
#  (at your option) any later version.
#
#  systemd is distributed in the hope that it will be useful, but
#  WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#  Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public License
#  along with systemd; If not, see <http://www.gnu.org/licenses/>.

set -e

verb="$1"

case "$verb" in
"") ;;
[cgals]) shift ;;
*) echo "Unexpected argument: $verb" >&2; exit 1 ;;
esac

oldpwd=$(pwd)
topdir=$(dirname $0)
cd $topdir

if [ -f .git/hooks/pre-commit.sample ] && [ ! -f .git/hooks/pre-commit ]; then
        # This part is allowed to fail
        cp -p .git/hooks/pre-commit.sample .git/hooks/pre-commit && \
        chmod +x .git/hooks/pre-commit && \
        echo "Activated pre-commit hook." || :
fi

intltoolize --force --automake
autoreconf --force --install --symlink
sed -r -e "s/^([[:space:]]+)(hardcode_libdir_flag_spec[_a-zA-Z0-9]*=[\"'](\\\$(\\{wl\\}|wl)){0,1}(\\-{1,2}(rpath|blibpath|R)|\\+b ).+)/\\1#\\2/" -e "s/^([[:space:]]+)(runpath_var=[\"']{0,1}LD_RUN_PATH[\"']{0,1})/\\1#\\2/" -e 's/^([[:space:]]+)(hardcode_into_libs=yes)/\1#\2/' -e 's#( \-install_name )\\\$rpath/#\1#g' -i configure

libdir() {
        echo $(cd "$1/$(gcc -print-multi-os-directory)"; pwd)
}

args="\
--sysconfdir=/etc \
--localstatedir=/var \
--libdir=$(libdir /usr/lib) \
"

if [ -f "$topdir/.config.args" ]; then
        args="$args $(cat $topdir/.config.args)"
fi

if [ ! -L /bin ]; then
args="$args \
--with-rootprefix=/ \
--with-rootlibdir=$(libdir /lib) \
"
fi

args="$args $@"
cd $oldpwd

if [ "$verb" = "c" ]; then
        set -x
        $topdir/configure CFLAGS='-g -O0 -ftrapv' $args
        make clean >/dev/null
elif [ "$verb" = "g" ]; then
        set -x
        $topdir/configure CFLAGS='-g -Og -ftrapv' $args
        make clean >/dev/null
elif [ "$verb" = "a" ]; then
        set -x
        $topdir/configure CFLAGS='-g -O0 -Wsuggest-attribute=pure -Wsuggest-attribute=const -ftrapv' $args
        make clean >/dev/null
elif [ "$verb" = "l" ]; then
        set -x
        $topdir/configure CC=clang CFLAGS='-g -O0 -ftrapv' $args
        make clean >/dev/null
elif [ "$verb" = "s" ]; then
        set -x
        scan-build $topdir/configure CFLAGS='-std=gnu99 -g -O0 -ftrapv' $args
        scan-build make
else
        echo
        echo "----------------------------------------------------------------"
        echo "Initialized build system. For a common configuration please run:"
        echo "----------------------------------------------------------------"
        echo
        echo "$topdir/configure CFLAGS='-g -O0 -ftrapv' $args"
        echo
fi
