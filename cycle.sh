#!/usr/bin/env bash

set -v
set -x
set -e

#build
cd /home/builder/mod-plugin-builder
rm -fr  /home/builder/mod-plugin-builder/plugins/package/alo
cp -r /tmp/moddevices/alo /home/builder/mod-plugin-builder/plugins/package
rm -fr /home/builder/mod-workdir/plugins-dep/build/alo* && ./build alo

# copy gui
cp -r /tmp/moddevices/alo/source/alo.lv2/modgui /home/builder/mod-workdir/plugins/alo.lv2

# deploy
cd /home/builder/mod-workdir/plugins && tar cz alo.lv2 | base64 | curl -F 'package=@-' http://192.168.51.1/sdk/install

cp -r /home/builder/mod-workdir/plugins/alo.lv2 /tmp/moddevices/lv2-test-area