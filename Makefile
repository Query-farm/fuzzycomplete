PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=fuzzycomplete
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

rust_binding_headers:
	cd duckdb_fuzzycomplete_rust && cbindgen --config ./cbindgen.toml --crate duckdb_fuzzycomplete_rust --output ../src/include/rust.h

clean_all: clean
	cd duckdb_fuzzycomplete_rust && cargo clean