cmake_minimum_required (VERSION 3.1) # 3.1 adds C_STANDARD
project (asjail LANGUAGES CXX)
add_executable (asjail asjail.cpp)
set_property (TARGET asjail PROPERTY CXX_STANDARD 14)
target_link_libraries (asjail -lsh)
install (TARGETS asjail DESTINATION sbin)
install (PROGRAMS asjail-max-1 DESTINATION sbin)
install (PROGRAMS asjail-max DESTINATION sbin)
