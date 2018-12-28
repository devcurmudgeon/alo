#!/usr/bin/env bash

set -v
set -x
set -e

#build
cd /home/builder/mod-plugin-builder
rm -fr /home/builder/mod-plugin-builder/plugins/package/alo
rm -fr /home/builder/mod-workdir/plugins-dep/target/usr/lib/lv2/alo.lv2
rm -fr /home/builder/mod-workdir/plugins-dep/build/alo*
rm -fr /home/builder/mod-workdir/plugins/alo.lv2
cp -r /tmp/moddevices/alo /home/builder/mod-plugin-builder/plugins/package
./build alo

# deploy
cd /home/builder/mod-workdir/plugins && tar cz alo.lv2 | base64 | curl -F 'package=@-' http://192.168.51.1/sdk/install
