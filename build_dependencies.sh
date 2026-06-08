#!/bin/bash
set -x
set -e
##############################
GITHUB_WORKSPACE="${PWD}"
ls -la ${GITHUB_WORKSPACE}
cd ${GITHUB_WORKSPACE}

BUILD_TESTS=false
for arg in "$@"; do
    if [ "$arg" = "--build-tests" ]; then
        BUILD_TESTS=true
        break
    fi
done

# # ############################# 
#1. Install Dependencies and packages

apt update
apt install -y libsqlite3-dev libcurl4-openssl-dev valgrind lcov clang libsystemd-dev libboost-all-dev libwebsocketpp-dev meson libcunit1 libcunit1-dev curl protobuf-compiler-grpc libgrpc-dev libgrpc++-dev libunwind-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
pip install jsonref

############################
# Build trevor-base64
if [ ! -d "trower-base64" ]; then
git clone --depth 1 https://github.com/xmidt-org/trower-base64.git
fi
cd trower-base64
meson setup --warnlevel 3 --werror build
ninja -C build
ninja -C build install
cd ..
###########################################
# Clone the required repositories


git clone --depth 1 --branch  R4.4.3 https://github.com/rdkcentral/ThunderTools.git

git clone --depth 1 --branch R4.4.1 https://github.com/rdkcentral/Thunder.git

git clone --depth 1 --branch develop https://github.com/rdkcentral/entservices-apis.git

git clone --depth 1 --branch $CTRLM_TAG https://github.com/rdkcentral/control.git

git clone --depth 1 https://$GITHUB_TOKEN@github.com/rdkcentral/entservices-testframework.git

############################
# Build Thunder-Tools
echo "======================================================================================"
echo "building thunderTools"
cd ThunderTools
patch -p1 < $GITHUB_WORKSPACE/entservices-testframework/patches/00010-R4.4-Add-support-for-project-dir.patch
cd -


cmake -G Ninja -S ThunderTools -B build/ThunderTools \
    -DEXCEPTIONS_ENABLE=ON \
    -DCMAKE_INSTALL_PREFIX="$GITHUB_WORKSPACE/install/usr" \
    -DCMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake" \
    -DGENERIC_CMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake"

cmake --build build/ThunderTools --target install


############################
# Build Thunder
echo "======================================================================================"
echo "building thunder"

cd Thunder
patch -p1 < $GITHUB_WORKSPACE/entservices-testframework/patches/Use_Legact_Alt_Based_On_ThunderTools_R4.4.3.patch
patch -p1 < $GITHUB_WORKSPACE/entservices-testframework/patches/error_code_R4_4.patch
patch -p1 < $GITHUB_WORKSPACE/entservices-testframework/patches/1004-Add-support-for-project-dir.patch
patch -p1 < $GITHUB_WORKSPACE/entservices-testframework/patches/RDKEMW-733-Add-ENTOS-IDS.patch
cd -

cmake -G Ninja -S Thunder -B build/Thunder \
    -DMESSAGING=ON \
    -DCMAKE_INSTALL_PREFIX="$GITHUB_WORKSPACE/install/usr" \
    -DCMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake" \
    -DGENERIC_CMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake" \
    -DBUILD_TYPE=Debug \
    -DBINDING=127.0.0.1 \
    -DPORT=55555 \
    -DEXCEPTIONS_ENABLE=ON

cmake --build build/Thunder --target install

############################
# Build entservices-apis
echo "======================================================================================"
echo "buliding entservices-apis"
cd entservices-apis
rm -rf jsonrpc/DTV.json
cd ..

cmake -G Ninja -S entservices-apis  -B build/entservices-apis \
    -DEXCEPTIONS_ENABLE=ON \
    -DCMAKE_INSTALL_PREFIX="$GITHUB_WORKSPACE/install/usr" \
    -DCMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake" \

cmake --build build/entservices-apis --target install

############################
# generating external headers
cd $GITHUB_WORKSPACE
cd entservices-testframework/Tests
echo " Empty mocks creation to avoid compilation errors"
echo "======================================================================================"
mkdir -p headers
mkdir -p headers/audiocapturemgr
mkdir -p headers/rdk/ds
mkdir -p headers/rdk/iarmbus
mkdir -p headers/rdk/iarmmgrs-hal
mkdir -p headers/ccec/drivers
mkdir -p headers/network
mkdir -p headers/proc
mkdir -p headers/websocket
echo "dir created successfully"
echo "======================================================================================"

echo "======================================================================================"
echo "empty headers creation"
cd headers
echo "current working dir: "${PWD}
touch audiocapturemgr/audiocapturemgr_iarm.h
touch ccec/drivers/CecIARMBusMgr.h
touch rdk/ds/audioOutputPort.hpp
touch rdk/ds/compositeIn.hpp
touch rdk/ds/dsDisplay.h
touch rdk/ds/dsError.h
touch rdk/ds/dsMgr.h
touch rdk/ds/dsTypes.h
touch rdk/ds/dsUtl.h
touch rdk/ds/exception.hpp
touch rdk/ds/hdmiIn.hpp
touch rdk/ds/host.hpp
touch rdk/ds/list.hpp
touch rdk/ds/manager.hpp
touch rdk/ds/sleepMode.hpp
touch rdk/ds/videoDevice.hpp
touch rdk/ds/videoOutputPort.hpp
touch rdk/ds/videoOutputPortConfig.hpp
touch rdk/ds/videoOutputPortType.hpp
touch rdk/ds/videoResolution.hpp
touch rdk/ds/frontPanelIndicator.hpp
touch rdk/ds/frontPanelConfig.hpp
touch rdk/ds/frontPanelTextDisplay.hpp
touch rdk/iarmbus/libIARM.h
touch rdk/iarmbus/libIBus.h
touch rdk/iarmbus/libIBusDaemon.h
touch rdk/iarmmgrs-hal/deepSleepMgr.h
touch rdk/iarmmgrs-hal/mfrMgr.h
touch rdk/iarmmgrs-hal/sysMgr.h
touch network/wifiSrvMgrIarmIf.h
touch network/netsrvmgrIarm.h
touch libudev.h
touch rfcapi.h
touch rbus.h
touch motionDetector.h
touch telemetry_busmessage_sender.h
touch maintenanceMGR.h
touch pkg.h
touch edid-parser.hpp
touch secure_wrapper.h
touch wpa_ctrl.h
touch btmgr.h
touch proc/readproc.h
touch rdk_logger_milestone.h
touch gdialservice.h
touch gdialservicecommon.h
touch rtRemote.h
touch rtObject.h
touch rtError.h
touch rtNotifier.h
touch dsFPD.h
# Copy ctrlm headers
cp ${GITHUB_WORKSPACE}/control/include/* .

echo "files created successfully"
echo "======================================================================================"

cd ../../
cp -r /usr/include/gstreamer-1.0/gst /usr/include/glib-2.0/* /usr/lib/x86_64-linux-gnu/glib-2.0/include/* /usr/local/include/trower-base64/base64.h /usr/include/libdrm/drm.h /usr/include/libdrm/drm_mode.h /usr/include/xf86drm.h .

############################
# Build test dependencies
if $BUILD_TESTS; then
    cd $GITHUB_WORKSPACE

    ############################
    # Build google test
    git clone --depth 1 --branch v1.15.0 https://github.com/google/googletest.git

    cmake -G Ninja -S "googletest" -B build/googletest \
          -DCMAKE_INSTALL_PREFIX="install/usr" \
          -DCMAKE_MODULE_PATH="install/tools/cmake" \
          -DGENERIC_CMAKE_MODULE_PATH="install/tools/cmake" \
          -DBUILD_TYPE=Debug \
          -DBUILD_GMOCK=ON \
          -DBUILD_SHARED_LIBS=OFF \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON

    cmake --build build/googletest -j8

    cmake --install build/googletest

    ############################
    # Build entservices-testframework (mocks)
    cmake -S "entservices-testframework/Tests/mocks" -B build/mocks \
          -DBUILD_SHARED_LIBS=ON \
          -DCMAKE_INSTALL_PREFIX="install/usr" \
          -DCMAKE_MODULE_PATH="install/tools/cmake" \
          -DCMAKE_CXX_FLAGS=" \
          -I entservices-testframework/Tests/headers \
          -I /usr/include/gstreamer-1.0 \
          -I /usr/include/glib-2.0 \
          -I /usr/lib/x86_64-linux-gnu/glib-2.0/include \
          -I /usr/include/libdrm \
          -I install/usr/include"

    cmake --build build/mocks -j8

    cmake --install build/mocks
fi
