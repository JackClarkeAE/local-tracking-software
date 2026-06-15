# FindK4A.cmake - Find Azure Kinect Sensor SDK
set(K4A_SDK_DIR "C:/Program Files/Azure Kinect SDK v1.4.1")

find_path(K4A_INCLUDE_DIR
    NAMES k4a/k4a.h
    PATHS "${K4A_SDK_DIR}/sdk/include"
)

find_library(K4A_LIBRARY
    NAMES k4a
    PATHS "${K4A_SDK_DIR}/sdk/windows-desktop/amd64/release/lib"
)

set(K4A_BIN_DIR "${K4A_SDK_DIR}/sdk/windows-desktop/amd64/release/bin")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(K4A DEFAULT_MSG K4A_LIBRARY K4A_INCLUDE_DIR)

if(K4A_FOUND)
    set(K4A_INCLUDE_DIRS ${K4A_INCLUDE_DIR})
    set(K4A_LIBRARIES ${K4A_LIBRARY})
endif()
