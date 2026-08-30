<p align="center">
  <a href="https://query.farm">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://query.farm/media-kit/logo/wordmark-dark.svg">
      <img alt="Query.Farm" src="https://query.farm/media-kit/logo/wordmark-light.svg" height="64">
    </picture>
  </a>
</p>

# fuzzycomplete Extension for DuckDB

[![DuckDB](https://img.shields.io/badge/DuckDB-community_extension-fdf1e0?logo=duckdb&logoColor=fff000)](https://duckdb.org/community_extensions/extensions/fuzzycomplete.html)
[![v1.5 build](https://github.com/Query-farm/fuzzycomplete/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=v1.5)](https://github.com/Query-farm/fuzzycomplete/actions/workflows/MainDistributionPipeline.yml?query=branch%3Av1.5)

This `fuzzycomplete` extension serves as an alternative to DuckDB's [autocomplete](https://duckdb.org/docs/api/cli/autocomplete.html) extension, using a fuzzy string matching algorithm derived from Visual Studio Code for more intuitive and flexible completion suggestions. It can complete table names across different databases and schemas, respecting the current search path even when multiple databases are attached.

## Documentation

Full documentation, including installation, usage, the function reference, and cookbook examples, is available at:

**[https://query.farm/products/extensions/fuzzycomplete](https://query.farm/products/extensions/fuzzycomplete)**

## Installation

```sql
install fuzzycomplete from community;
load fuzzycomplete;
```

## Development

For instructions on building the extension from source and running its tests, see [BUILDING.md](BUILDING.md).
