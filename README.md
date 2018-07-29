

docker run -ti --name mpb -p 9000:9000 -v ~/Projects/2018/moddevices/:/tmp/aloo-lv2 moddevices/mod-plugin-builder

# build
cd /home/builder/mod-plugin-builder
rm -fr  /home/builder/mod-plugin-builder/plugins/package/alo
cp -r /tmp/moddevices/alo /home/builder/mod-plugin-builder/plugins/package
rm -fr /home/builder/mod-workdir/plugins-dep/build/alo* && ./build alo

# deploy
cd /home/builder/mod-workdir/plugins && tar cz alo.lv2 | base64 | curl -F 'package=@-' http://192.168.51.1/sdk/install

#debug
cat /root/alo.log

mod-host -p 1234 -i
add http://devcurmudgeon.com/alo 0
