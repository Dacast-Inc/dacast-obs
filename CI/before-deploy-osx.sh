hr() {
  echo "───────────────────────────────────────────────────"
  echo $1
  echo "───────────────────────────────────────────────────"
}

# Exit if something fails
set -e

# Generate file name variables
#export GIT_TAG=$(git describe --abbrev=0)
export GIT_TAG=$(git describe --tags)
export GIT_HASH=$(git rev-parse --short HEAD)
export FILE_DATE=$(date +%Y-%m-%d.%H-%M-%S)
export FILENAME=$FILE_DATE-$GIT_HASH-$TRAVIS_BRANCH-osx.dmg

echo "git tag: $GIT_TAG"

cd ./build

# Package everything into a nice .app
hr "Packaging .app"
STABLE=false
if [ -n "${TRAVIS_TAG}" ]; then
  STABLE=true
fi

#sudo python ../CI/install/osx/build_app.py --public-key ../CI/install/osx/OBSPublicDSAKey.pem --sparkle-framework ../../sparkle/Sparkle.framework --stable=$STABLE

../CI/install/osx/packageApp.sh

# fix obs outputs plugin it doesn't play nicely with dylibBundler at the moment
hr "Copying Sparkle.framework"
cp /usr/local/opt/mbedtls/lib/libmbedtls.12.dylib ./OBS.app/Contents/Frameworks/
cp /usr/local/opt/mbedtls/lib/libmbedcrypto.3.dylib ./OBS.app/Contents/Frameworks/
cp /usr/local/opt/mbedtls/lib/libmbedx509.0.dylib ./OBS.app/Contents/Frameworks/
chmod +w ./OBS.app/Contents/Frameworks/*.dylib
install_name_tool -id @executable_path/../Frameworks/libmbedtls.12.dylib ./OBS.app/Contents/Frameworks/libmbedtls.12.dylib
install_name_tool -id @executable_path/../Frameworks/libmbedcrypto.3.dylib ./OBS.app/Contents/Frameworks/libmbedcrypto.3.dylib
install_name_tool -id @executable_path/../Frameworks/libmbedx509.0.dylib ./OBS.app/Contents/Frameworks/libmbedx509.0.dylib
install_name_tool -change libmbedtls.12.dylib @executable_path/../Frameworks/libmbedtls.12.dylib ./OBS.app/Contents/Plugins/obs-outputs.so
install_name_tool -change libmbedcrypto.3.dylib @executable_path/../Frameworks/libmbedcrypto.3.dylib ./OBS.app/Contents/Plugins/obs-outputs.so
install_name_tool -change libmbedx509.0.dylib @executable_path/../Frameworks/libmbedx509.0.dylib ./OBS.app/Contents/Plugins/obs-outputs.so
install_name_tool -change /usr/local/opt/curl/lib/libcurl.4.dylib @executable_path/../Frameworks/libcurl.4.dylib ./OBS.app/Contents/Plugins/obs-outputs.so
install_name_tool -change @rpath/libobs.0.dylib @executable_path/../Frameworks/libobs.0.dylib ./OBS.app/Contents/Plugins/obs-outputs.so
install_name_tool -change /tmp/obsdeps/bin/libjansson.4.dylib @executable_path/../Frameworks/libjansson.4.dylib ./OBS.app/Contents/Plugins/obs-outputs.so

# copy sparkle into the app
hr "Copying Sparkle.framework"
cp -R ../../sparkle/Sparkle.framework ./OBS.app/Contents/Frameworks/
install_name_tool -change @rpath/Sparkle.framework/Versions/A/Sparkle @executable_path/../Frameworks/Sparkle.framework/Versions/A/Sparkle ./OBS.app/Contents/MacOS/obs

cp ../CI/install/osx/OBSPublicDSAKey.pem OBS.app/Contents/Resources

# edit plist
hr "Edit plist"
plutil -insert CFBundleVersion -string $GIT_TAG ./OBS.app/Contents/Info.plist
plutil -insert CFBundleShortVersionString -string $GIT_TAG ./OBS.app/Contents/Info.plist
plutil -insert OBSFeedsURL -string https://obsproject.com/osx_update/feeds.xml ./OBS.app/Contents/Info.plist
plutil -insert SUFeedURL -string https://obsproject.com/osx_update/stable/updates.xml ./OBS.app/Contents/Info.plist
plutil -insert SUPublicDSAKeyFile -string OBSPublicDSAKey.pem ./OBS.app/Contents/Info.plist

hr "Generate dmg"
dmgbuild -s ../CI/install/osx/settings.json "OBS" obs.dmg
