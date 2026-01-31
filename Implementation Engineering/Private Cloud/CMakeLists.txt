




find_package(OpenSSL REQUIRED)
target_link_libraries(your_target OpenSSL::SSL OpenSSL::Crypto)
# nlohmann_json via FetchContent or submodule




add_library(pugixml pugixml.cpp)
target_include_directories(pugixml PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml)
target_link_libraries(your_target pugixml)



find_library(LEVELDB_LIBRARY leveldb)
find_path(LEVELDB_INCLUDE_DIR leveldb/db.h)
target_include_directories(your_target PRIVATE ${LEVELDB_INCLUDE_DIR})
target_link_libraries(your_target ${LEVELDB_LIBRARY})




