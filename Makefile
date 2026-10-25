PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=quackapi
EXT_CONFIG=${PROJ_DIR}extension_config.cmake
DUCKDB_GIT_VERSION ?= v2.0.0
OVERRIDE_GIT_DESCRIBE ?= v2.0.0

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Companion extensions must be in the repository used by each isolated test
# database. Query.Farm repository discovery needs native httpfs on DuckDB 2.0.
ifneq ($(SKIP_TESTS),1)
test_release: native_companions_release
test_debug: native_companions_debug
test_reldebug: native_companions_reldebug
endif

INSTALL_NATIVE_COMPANIONS = ./build/$(1)/duckdb -no-agent -c \
	"SET extension_directory = '$(PROJ_DIR)build/$(1)/repository'; INSTALL httpfs; INSTALL quack;"

native_companions_release:
	$(call INSTALL_NATIVE_COMPANIONS,release)
native_companions_debug:
	$(call INSTALL_NATIVE_COMPANIONS,debug)
native_companions_reldebug:
	$(call INSTALL_NATIVE_COMPANIONS,reldebug)

.PHONY: native_companions_release native_companions_debug native_companions_reldebug

ifneq ($(TEST_RUNNER),)
TEST_RUNNER += --fail-require-skip
endif

# Assert the artifact built by this checkout even when tests enable autoloading.
QUACKAPI_BUILT_VERSION = $(shell ./build/release/duckdb -unsigned -no-agent -noheader -list -c \
	"LOAD '$(PROJ_DIR)build/release/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension'; \
	 SELECT extension_version FROM duckdb_extensions() WHERE extension_name='$(EXT_NAME)';" 2>/dev/null | tail -1)
export EXPECTED_QUACKAPI_VERSION = $(QUACKAPI_BUILT_VERSION)

# The Code Quality job formats with clang-format 11.0.1 and black 24 (extension-ci-tools
# _extension_code_quality.yml). Any other clang-format wraps differently, and a machine with
# none cannot run `make format-check` at all, so these run format.py with CI's exact tools.
FORMAT_WITH_CI_TOOLS = uv run --no-project --with 'clang_format==11.0.1' --with 'black==24.*' \
	--with cmake-format --with cxxheaderparser --with pcpp python3 duckdb/scripts/format.py --all

format-ci:
	$(FORMAT_WITH_CI_TOOLS) --fix --noconfirm --directories src test

format-ci-check:
	$(FORMAT_WITH_CI_TOOLS) --check --directories src test

.PHONY: format-ci format-ci-check
