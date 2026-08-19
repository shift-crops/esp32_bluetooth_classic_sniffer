#!/usr/bin/env bash

echo -e "\nCompiling BT host programs (against pristine host_stack/btstack + host_stack/overlay patches)"
sudo apt install cmake software-properties-common -y
mkdir -p build
cd build
cmake ../
make -j
cd ../

echo -e "\n\nBuilding external h4bcm dissector"
cd dissectors
./build.sh
cd ../

echo "Done!"
