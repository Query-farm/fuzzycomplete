# fuzzycomplete Extension for DuckDB

![A duck trying to complete a crossword puzzle](./docs/duckdb-fuzzycompletion.jpeg)

This `fuzzycomplete` extension serves as an alternative to DuckDB's [autocomplete](https://duckdb.org/docs/api/cli/autocomplete.html) extension, with several key differences:

**Algorithm:** Unlike the [autocomplete extension](https://duckdb.org/docs/extensions/autocomplete.html), which uses edit distance as its metric, the fuzzycomplete extension employs a fuzzy string matching algorithm derived from Visual Studio Code. This provides more intuitive and flexible completion suggestions.

**Scope:** The `fuzzycomplete` extension can complete table names across different databases and schemas. It respects the current search path and offers suggestions accordingly, even when multiple databases are attached.

It may not yet be the best solution for SQL completion, but it has proven to be useful to the author.

## Installation

**`fuzzycomplete` is a [DuckDB Community Extension](https://github.com/duckdb/community-extensions).**

You can now use this by using this SQL:

```sql
install fuzzycomplete from community;
load fuzzycomplete;
```

## Details of the fuzzy matching algorithm

This extension uses the Rust crate [`code-fuzzy-match`](https://crates.io/crates/code-fuzzy-match)

The algorithm ensures that characters in the query string appear in the same order in the target string. It handles substring queries efficiently, allowing searches within the middle of the target string without significantly impacting the match score. The algorithm prioritizes matches that occur at the beginning of words, where words are defined as they commonly appear in code (e.g., letters following a separator or in camel case). Sequential matches are also given preference.

In addition to the basic matching algorithm, matches then scored using this criteria if they have an equal score from `code-fuzzy-match`:

1. In the event of a tie in the match score, completion results are first ordered by the number of pseudo-words in the candidate strings, favoring shorter completions.
2. A standard lexical sorting is then applied.

## When would I use this?

If you're looking to try a different completion algorithm or need to complete table names from various databases and schemas, you might find this extension beneficial.

### Build Architecture

For the DuckDB extension to call the Rust code a tool called `cbindgen` is used to write the C++ headers for the exposed Rust interface.

The headers can be updated by running `make rust_binding_headers`.

### Build steps
Now to build the extension, run:
```sh
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

## Running the extension
To run the extension code, simply start the shell with `./build/release/duckdb`.

Now we can use the features from the extension directly in DuckDB.

### Installing the deployed binaries
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
