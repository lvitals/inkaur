#!/bin/bash

# Ensure the project and tests are compiled
make clean all tests/run_tests

echo "--- Checking Test Suite for Memory Leaks ---"
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --verbose \
         --log-file=valgrind_tests.log \
         ./tests/run_tests

echo "--- Checking inkaur -Ss hello (Search) ---"
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --log-file=valgrind_search.log \
         ./inkaur -Ss hello > /dev/null

echo "--- Checking inkaur -Si hello (Info) ---"
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --log-file=valgrind_info.log \
         ./inkaur -Si hello > /dev/null

echo ""
echo "Valgrind logs generated:"
echo "  - valgrind_tests.log"
echo "  - valgrind_search.log"
echo "  - valgrind_info.log"

# Summary of errors found
grep "ERROR SUMMARY" valgrind_*.log
