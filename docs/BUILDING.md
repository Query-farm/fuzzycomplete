# Building the extension

```sh
# Clone this repo with submodules.
# duckdb and extension-ci-tools are submodules.
git clone --recurse-submodules git@github.com:Query-farm/fuzzycomplete

# Build the extension
make
```

The main binaries that will be built are:
```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/fuzzycomplete/fuzzycomplete.duckdb_extension
```
- `duckdb` is the binary for the duckdb shell with the extension code automatically loaded.
- `unittest` is the test runner of duckdb. Again, the extension is already linked into the binary.
- `fuzzycomplete.duckdb_extension` is the loadable binary as it would be distributed.

## Build Architecture

For the DuckDB extension to call the Rust code a tool called `cbindgen` is used to write the C++ headers for the exposed Rust interface.

The headers can be updated by running `make rust_binding_headers`.

## Running the extension

To run the extension code, simply start the shell with `./build/release/duckdb`.

Now we can use the features from the extension directly in DuckDB.

## Installing the deployed binaries

To install your extension binaries from S3, you will need to do two things. Firstly, DuckDB should be launched with the
`allow_unsigned_extensions` option set to true. How to set this will depend on the client you're using. Some examples:

CLI:
```shell
duckdb -unsigned
```

Python:
```python
con = duckdb.connect(':memory:', config={'allow_unsigned_extensions' : 'true'})
```

NodeJS:
```js
db = new duckdb.Database(':memory:', {"allow_unsigned_extensions": "true"});
```

Secondly, you will need to set the repository endpoint in DuckDB to the HTTP url of your bucket + version of the extension
you want to install. To do this run the following SQL query in DuckDB:
```sql
SET custom_extension_repository='bucket.s3.us-east-1.amazonaws.com/fuzzycomplete/latest';
```
Note that the `/latest` path will allow you to install the latest extension version available for your current version of
DuckDB. To specify a specific version, you can pass the version instead.

After running these steps, you can install and load your extension using the regular INSTALL/LOAD commands in DuckDB:
```sql
INSTALL fuzzycomplete
LOAD fuzzycomplete
```
