include_guard(GLOBAL)

function(_check_deps_version version)
  set(found FALSE)

  foreach(path IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${path}/share/obs-deps/VERSION")
      file(READ "${path}/share/obs-deps/VERSION" _check_version)
      string(REPLACE "\n" "" _check_version "${_check_version}")
      string(REPLACE "-" "." _check_version "${_check_version}")
      string(REPLACE "-" "." version "${version}")

      if(_check_version VERSION_GREATER_EQUAL version)
        set(found TRUE)
        break()
      endif()
    endif()
  endforeach()

  return(PROPAGATE found)
endfunction()

function(_setup_obs_studio)
  if(NOT libobs_DIR)
    set(_is_fresh --fresh)
  endif()

  set(_cmake_generator "${CMAKE_GENERATOR}")
  set(_cmake_arch "-A ${arch},version=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
  set(_cmake_extra "-DCMAKE_SYSTEM_VERSION=${CMAKE_SYSTEM_VERSION} -DCMAKE_ENABLE_SCRIPTING=OFF")

  message(STATUS "Configure ${label} (${arch})")
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -S "${dependencies_dir}/${_obs_destination}" -B
      "${dependencies_dir}/${_obs_destination}/build_${arch}" -G ${_cmake_generator} "${_cmake_arch}"
      -DOBS_CMAKE_VERSION:STRING=3.0.0 -DENABLE_PLUGINS:BOOL=OFF -DENABLE_FRONTEND:BOOL=OFF
      -DOBS_VERSION_OVERRIDE:STRING=${_obs_version} "-DCMAKE_PREFIX_PATH='${CMAKE_PREFIX_PATH}'" ${_is_fresh}
      ${_cmake_extra}
    RESULT_VARIABLE _process_result
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )

  message(STATUS "Build libobs development targets (${arch})")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build build_${arch} --target obs-frontend-api --config Release --parallel
    WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )

  message(STATUS "Install libobs development targets (${arch})")
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" --install build_${arch} --component Development --config Release --prefix "${dependencies_dir}"
    WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )
endfunction()

function(_check_dependencies)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" buildspec)
  string(JSON dependency_data GET ${buildspec} dependencies)

  foreach(dependency IN LISTS dependencies_list)
    string(JSON data GET ${dependency_data} ${dependency})
    string(JSON version GET ${data} version)
    string(JSON hash GET ${data} hashes ${platform})
    string(JSON url GET ${data} baseUrl)
    string(JSON label GET ${data} label)

    set(file "${${dependency}_filename}")
    set(destination "${${dependency}_destination}")
    string(REPLACE "VERSION" "${version}" file "${file}")
    string(REPLACE "VERSION" "${version}" destination "${destination}")
    string(REPLACE "ARCH" "${arch}" file "${file}")
    string(REPLACE "ARCH" "${arch}" destination "${destination}")
    string(REPLACE "_REVISION" "" file "${file}")
    string(REPLACE "-REVISION" "" file "${file}")

    if(EXISTS "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256")
      file(READ "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256" OBS_DEPENDENCY_HASH)
    else()
      set(OBS_DEPENDENCY_HASH "")
    endif()

    set(skip FALSE)
    if(dependency STREQUAL prebuilt OR dependency STREQUAL qt6)
      if(OBS_DEPENDENCY_HASH STREQUAL ${hash})
        _check_deps_version(${version})
        if(found)
          set(skip TRUE)
        endif()
      endif()
    endif()

    if(NOT skip)
      if(dependency STREQUAL obs-studio)
        set(download_url "${url}/${file}")
      else()
        set(download_url "${url}/${version}/${file}")
      endif()

      file(MAKE_DIRECTORY "${dependencies_dir}")
      if(NOT EXISTS "${dependencies_dir}/${file}")
        message(STATUS "Downloading ${label}: ${download_url}")
        file(
          DOWNLOAD "${download_url}" "${dependencies_dir}/${file}"
          STATUS download_status
          EXPECTED_HASH SHA256=${hash}
          SHOW_PROGRESS
        )
        list(GET download_status 0 error_code)
        list(GET download_status 1 error_message)
        if(error_code GREATER 0)
          file(REMOVE "${dependencies_dir}/${file}")
          message(FATAL_ERROR "Unable to download ${download_url}: ${error_message}")
        endif()
      endif()

      if(NOT OBS_DEPENDENCY_HASH STREQUAL ${hash})
        file(REMOVE_RECURSE "${dependencies_dir}/${destination}")
      endif()

      if(NOT EXISTS "${dependencies_dir}/${destination}")
        file(MAKE_DIRECTORY "${dependencies_dir}/${destination}")
        if(dependency STREQUAL obs-studio)
          file(ARCHIVE_EXTRACT INPUT "${dependencies_dir}/${file}" DESTINATION "${dependencies_dir}")
        else()
          file(ARCHIVE_EXTRACT INPUT "${dependencies_dir}/${file}" DESTINATION "${dependencies_dir}/${destination}")
        endif()
      endif()

      file(WRITE "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256" "${hash}")
    endif()

    if(dependency STREQUAL prebuilt OR dependency STREQUAL qt6)
      list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}/${destination}")
    elseif(dependency STREQUAL obs-studio)
      set(_obs_version ${version})
      set(_obs_destination "${destination}")
      list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}")
    endif()
  endforeach()

  list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
  set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} CACHE PATH "CMake prefix search path" FORCE)

  _setup_obs_studio()
endfunction()
