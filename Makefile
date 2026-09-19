PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=quackapi
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# The stock target trusts Catch's aggregate even when one SQLLogic file stops
# before its high-cardinality tail. Reconcile the file set and totals after it.
test_release_internal:
	python3 scripts/run_sql_tests.py build/release/test/unittest
