#!/bin/bash

if [ ! -f "build/contract-spike-diff" ] || [ ! -f "build/libcontract_spike_atom.so" ]; then
    echo "Executable or shared library not found. Building..."
    mkdir -p build
    nix-shell -p dtc --run "cd build && ../configure && make -j4 contract-spike-diff libcontract_spike_atom.so"
fi

start_time=$(date +%s%N)

./build/contract-spike-diff \
    --testcases 10000-IBEX-base-testcases.json \
    --all | jq > new.json
    # --case-index 10526 | jq

end_time=$(date +%s%N)

elapsed=$(( (end_time - start_time) / 1000000 ))
echo "Elapsed time: ${elapsed} ms"
