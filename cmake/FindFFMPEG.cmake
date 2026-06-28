#[=======================================================================[.rst:
FindFFMPEG
----------

Find the FFmpeg libraries.

Imported Targets
^^^^^^^^^^^^^^^^

``FFMPEG::avformat``
``FFMPEG::avcodec``
``FFMPEG::avutil``
``FFMPEG::swscale``
``FFMPEG::swresample``

Result Variables
^^^^^^^^^^^^^^^^

``FFMPEG_FOUND``
  True if FFmpeg was found.
``FFMPEG_INCLUDE_DIRS``
  Include directories needed to use FFmpeg.
``FFMPEG_LIBRARIES``
  Libraries needed to link to FFmpeg.
``FFMPEG_LIBRARY_DIRS``
  Library directories.

Hints
^^^^^

``FFMPEG_ROOT``
  Path to FFmpeg installation prefix.

#]=======================================================================]

include(FindPackageHandleStandardArgs)
include(CheckCXXSymbolExists)

# Use pkg-config if available
find_package(PkgConfig QUIET)

set(_FFMPEG_COMPONENTS avformat avcodec avutil swscale swresample)

set(FFMPEG_INCLUDE_DIRS "")
set(FFMPEG_LIBRARIES "")
set(FFMPEG_LIBRARY_DIRS "")

foreach(_comp IN LISTS _FFMPEG_COMPONENTS)
    if(PkgConfig_FOUND)
        pkg_check_modules(PC_${_comp} QUIET lib${_comp})
    endif()

    find_path(${_comp}_INCLUDE_DIR
        NAMES lib${_comp}/${_comp}.h
        HINTS
            # Prefer FFMPEG_ROOT (the stable Homebrew opt symlink) over the
            # versioned Cellar realpath returned by pkg-config, so the cached
            # path survives a `brew upgrade`.
            ${FFMPEG_ROOT}
            $ENV{FFMPEG_ROOT}
            ${PC_${_comp}_INCLUDEDIR}
            ${PC_${_comp}_INCLUDE_DIRS}
        PATH_SUFFIXES include
    )

    find_library(${_comp}_LIBRARY
        NAMES ${_comp}
        HINTS
            ${FFMPEG_ROOT}
            $ENV{FFMPEG_ROOT}
            ${PC_${_comp}_LIBDIR}
            ${PC_${_comp}_LIBRARY_DIRS}
        PATH_SUFFIXES lib lib64
    )

    if(${_comp}_INCLUDE_DIR)
        list(APPEND FFMPEG_INCLUDE_DIRS ${${_comp}_INCLUDE_DIR})
    endif()

    if(${_comp}_LIBRARY)
        list(APPEND FFMPEG_LIBRARIES ${${_comp}_LIBRARY})
        get_filename_component(_libdir ${${_comp}_LIBRARY} DIRECTORY)
        list(APPEND FFMPEG_LIBRARY_DIRS ${_libdir})
    endif()
endforeach()

# Deduplicate
if(FFMPEG_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIRS)
endif()
if(FFMPEG_LIBRARY_DIRS)
    list(REMOVE_DUPLICATES FFMPEG_LIBRARY_DIRS)
endif()

find_package_handle_standard_args(FFMPEG
    REQUIRED_VARS FFMPEG_LIBRARIES FFMPEG_INCLUDE_DIRS
)

if(FFMPEG_FOUND AND NOT TARGET FFMPEG::avformat)
    foreach(_comp IN LISTS _FFMPEG_COMPONENTS)
        if(${_comp}_LIBRARY AND NOT TARGET FFMPEG::${_comp})
            add_library(FFMPEG::${_comp} UNKNOWN IMPORTED)
            set_target_properties(FFMPEG::${_comp} PROPERTIES
                IMPORTED_LOCATION "${${_comp}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${${_comp}_INCLUDE_DIR}"
            )
        endif()
    endforeach()
endif()

mark_as_advanced(
    ${_FFMPEG_COMPONENTS}
    avformat_INCLUDE_DIR avformat_LIBRARY
    avcodec_INCLUDE_DIR avcodec_LIBRARY
    avutil_INCLUDE_DIR avutil_LIBRARY
    swscale_INCLUDE_DIR swscale_LIBRARY
    swresample_INCLUDE_DIR swresample_LIBRARY
)
