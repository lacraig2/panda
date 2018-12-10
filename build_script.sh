#!/bin/bash


progress() {
  echo
  echo -e "\e[32m[panda_install]\e[0m \e[1m$1\e[0m"
}


rm -rf ./build
progress "Building PANDA..."
mkdir build
cd build
../build.sh "$@"

cd ..
gcc -g -o papi panda_api_sample.c ./build/i386-softmmu/libpanda-i386.so
