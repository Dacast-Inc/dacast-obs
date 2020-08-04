# Exit if something fails
set -e

#set application bundle name
appName=DACAST-OBS.app

rm -rf $appName

mkdir $appName
mkdir $appName/Contents
mkdir $appName/Contents/MacOS
mkdir $appName/Contents/PlugIns
mkdir $appName/Contents/Resources

cp -R rundir/RelWithDebInfo/bin/ ./$appName/Contents/MacOS
cp -R rundir/RelWithDebInfo/data ./$appName/Contents/Resources
cp ../CI/install/osx/obs.icns ./$appName/Contents/Resources
cp -R rundir/RelWithDebInfo/obs-plugins/ ./$appName/Contents/PlugIns
cp ../CI/install/osx/Info.plist ./$appName/Contents

../CI/install/osx/dylibBundler -b -cd -d ./$appName/Contents/Frameworks -p @executable_path/../Frameworks/ \
-s ./$appName/Contents/MacOS \
-s /usr/local/opt/mbedtls/lib/ \
-x ./$appName/Contents/PlugIns/coreaudio-encoder.so \
-x ./$appName/Contents/PlugIns/frontend-tools.so \
-x ./$appName/Contents/PlugIns/image-source.so \
-x ./$appName/Contents/PlugIns/linux-jack.so \
-x ./$appName/Contents/PlugIns/mac-avcapture.so \
-x ./$appName/Contents/PlugIns/mac-capture.so \
-x ./$appName/Contents/PlugIns/mac-decklink.so \
-x ./$appName/Contents/PlugIns/mac-syphon.so \
-x ./$appName/Contents/PlugIns/mac-vth264.so \
-x ./$appName/Contents/PlugIns/obs-ffmpeg.so \
-x ./$appName/Contents/PlugIns/obs-filters.so \
-x ./$appName/Contents/PlugIns/obs-transitions.so \
-x ./$appName/Contents/PlugIns/obs-vst.so \
-x ./$appName/Contents/PlugIns/rtmp-services.so \
-x ./$appName/Contents/MacOS/obs \
-x ./$appName/Contents/MacOS/obs-ffmpeg-mux \
-x ./$appName/Contents/PlugIns/obs-x264.so \
-x ./$appName/Contents/PlugIns/text-freetype2.so \
-x ./$appName/Contents/PlugIns/obs-libfdk.so

/usr/local/Cellar/qt/5.14.1/bin/macdeployqt ./$appName

mv ./$appName/Contents/MacOS/libobs-opengl.so ./$appName/Contents/Frameworks

rm -f -r ./$appName/Contents/Frameworks/QtNetwork.framework

# put qt network in here becasuse streamdeck uses it
cp -R /usr/local/opt/qt/lib/QtNetwork.framework ./$appName/Contents/Frameworks
chmod -R +w ./$appName/Contents/Frameworks/QtNetwork.framework
rm -r ./$appName/Contents/Frameworks/QtNetwork.framework/Headers
rm -r ./$appName/Contents/Frameworks/QtNetwork.framework/Versions/5/Headers/
chmod 644 ./$appName/Contents/Frameworks/QtNetwork.framework/Versions/5/Resources/Info.plist
install_name_tool -id @executable_path/../Frameworks/QtNetwork.framework/Versions/5/QtNetwork ./$appName/Contents/Frameworks/QtNetwork.framework/Versions/5/QtNetwork
install_name_tool -change /usr/local/Cellar/qt/5.14.1/lib/QtCore.framework/Versions/5/QtCore @executable_path/../Frameworks/QtCore.framework/Versions/5/QtCore ./$appName/Contents/Frameworks/QtNetwork.framework/Versions/5/QtNetwork

# frontend tools qt
install_name_tool -change /usr/local/opt/qt/lib/QtGui.framework/Versions/5/QtGui @executable_path/../Frameworks/QtGui.framework/Versions/5/QtGui ./$appName/Contents/PlugIns/frontend-tools.so
install_name_tool -change /usr/local/opt/qt/lib/QtCore.framework/Versions/5/QtCore @executable_path/../Frameworks/QtCore.framework/Versions/5/QtCore ./$appName/Contents/PlugIns/frontend-tools.so
install_name_tool -change /usr/local/opt/qt/lib/QtWidgets.framework/Versions/5/QtWidgets @executable_path/../Frameworks/QtWidgets.framework/Versions/5/QtWidgets ./$appName/Contents/PlugIns/frontend-tools.so

# vst qt
install_name_tool -change /usr/local/opt/qt/lib/QtGui.framework/Versions/5/QtGui @executable_path/../Frameworks/QtGui.framework/Versions/5/QtGui ./$appName/Contents/PlugIns/obs-vst.so
install_name_tool -change /usr/local/opt/qt/lib/QtCore.framework/Versions/5/QtCore @executable_path/../Frameworks/QtCore.framework/Versions/5/QtCore ./$appName/Contents/PlugIns/obs-vst.so
install_name_tool -change /usr/local/opt/qt/lib/QtWidgets.framework/Versions/5/QtWidgets @executable_path/../Frameworks/QtWidgets.framework/Versions/5/QtWidgets ./$appName/Contents/PlugIns/obs-vst.so
install_name_tool -change /usr/local/opt/qt/lib/QtMacExtras.framework/Versions/5/QtMacExtras @executable_path/../Frameworks/QtMacExtras.framework/Versions/5/QtMacExtras ./$appName/Contents/PlugIns/obs-vst.so
