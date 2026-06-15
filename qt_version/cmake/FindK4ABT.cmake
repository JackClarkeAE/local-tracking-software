# FindK4ABT.cmake - Find Azure Kinect Body Tracking SDK
set(K4ABT_SDK_DIR "C:/Program Files/Azure Kinect Body Tracking SDK")

find_path(K4ABT_INCLUDE_DIR
    NAMES k4abt.h
    PATHS "${K4ABT_SDK_DIR}/sdk/include"
)

find_library(K4ABT_LIBRARY
    NAMES k4abt
    PATHS "${K4ABT_SDK_DIR}/sdk/windows-desktop/amd64/release/lib"
)

set(K4ABT_BIN_DIR "${K4ABT_SDK_DIR}/sdk/windows-desktop/amd64/release/bin")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(K4ABT DEFAULT_MSG K4ABT_LIBRARY K4ABT_INCLUDE_DIR)

if(K4ABT_FOUND)
    set(K4ABT_INCLUDE_DIRS ${K4ABT_INCLUDE_DIR})
    set(K4ABT_LIBRARIES ${K4ABT_LIBRARY})
endif()
