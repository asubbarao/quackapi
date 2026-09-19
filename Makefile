PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=quackapi
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# A source checkout must use the same pinned vcpkg commit as distribution CI;
# otherwise a clean release silently falls back to host libraries.
VCPKG_EXTERNAL_TOOLCHAIN := $(VCPKG_TOOLCHAIN_PATH)
ifeq ($(strip $(VCPKG_EXTERNAL_TOOLCHAIN)),)
VCPKG_TOOLCHAIN_PATH := $(PROJ_DIR)vcpkg/scripts/buildsystems/vcpkg.cmake
VCPKG_BOOTSTRAP_TARGET := vcpkg/scripts/buildsystems/vcpkg.cmake
endif

ifndef VCPKG_TARGET_TRIPLET
VCPKG_HOST_OS := $(shell uname -s)
VCPKG_HOST_ARCH := $(shell uname -m)
ifeq ($(VCPKG_HOST_OS),Darwin)
VCPKG_TARGET_TRIPLET := $(if $(filter arm64 aarch64,$(VCPKG_HOST_ARCH)),arm64-osx-release,x64-osx-release)
else ifeq ($(VCPKG_HOST_OS),Linux)
VCPKG_TARGET_TRIPLET := $(if $(filter arm64 aarch64,$(VCPKG_HOST_ARCH)),arm64-linux-release,x64-linux-release)
endif
endif
VCPKG_HOST_TRIPLET ?= $(VCPKG_TARGET_TRIPLET)

# The release matrix supplies its own already-bootstrapped toolchain. Local
# source builds bootstrap the pinned toolchain so `make release` has one path.
include extension-ci-tools/makefiles/vcpkg.Makefile

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

ifneq ($(strip $(VCPKG_BOOTSTRAP_TARGET)),)
release debug reldebug relassert: $(VCPKG_BOOTSTRAP_TARGET)
endif
