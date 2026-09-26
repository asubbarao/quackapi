PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=quackapi
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

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
