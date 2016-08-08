add_executable (lru_map_test lru_map_test.cpp)

include_directories (..)
include_directories (/usr/local/include)

find_library (glog_library glog HINTS /usr/local/lib)
target_link_libraries (lru_map_test PUBLIC ${glog_library})
target_link_libraries (lru_map_test PUBLIC pthread)
target_link_libraries (lru_map_test PUBLIC unwind)
