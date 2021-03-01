find_path(NLOHMANNJSON_INCLUDE_DIR nlohmann/json.hpp)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(nlohmann_json DEFAULT_MSG NLOHMANNJSON_INCLUDE_DIR)

if(nlohmann_json_FOUND)
  set(NLOHMANNJSON_INCLUDE_DIRS ${NLOHMANNJSON_INCLUDE_DIR})
endif()

mark_as_advanced(NLOHMANNJSON_INCLUDE_DIR)

