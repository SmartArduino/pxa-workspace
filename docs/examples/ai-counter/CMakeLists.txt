cmake_minimum_required(VERSION 3.20)
project(hello LANGUAGES CXX)
find_package(Pxa CONFIG REQUIRED)
pxa_add_app(main SOURCES main.cpp)
