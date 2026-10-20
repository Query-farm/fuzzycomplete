// duckdb_fuzzycomplete_rust
// Copyright 2024 Rusty Conover <rusty@conover.me>
// Licensed under the MIT License

use code_fuzzy_match;

use core::str;
use std::ffi::CStr;
use std::{ffi::c_char, slice};

macro_rules! make_str {
    ( $s : expr , $len : expr ) => {
        unsafe { str::from_utf8_unchecked(slice::from_raw_parts($s as *const u8, $len)) }
    };
}

#[no_mangle]
pub extern "C" fn perform_matches(
    // These are the array of strings that we are going to match against.
    candidate_pool: *const *const c_char,
    candidate_pool_size: usize,

    // This is the query sting to match
    query: *const c_char,
    query_len: usize,

    // The maximum number of results to return
    max_results: usize,

    // The output ranking of candidates (pointers from the candidate pool)
    ranked_candidates: *mut *const c_char,

    // The fuzzy match score of each ranked candidate (higher is better), may be null.
    ranked_scores: *mut u64,

    // The actual number of produced results.
    actual_results: *mut usize,
) {
    let mut matcher = code_fuzzy_match::FuzzyMatcher::new();

    let candidates: Vec<_> = (0..candidate_pool_size).map(|i| unsafe {
        let c_str_ptr = *candidate_pool.add(i);
        (CStr::from_ptr(c_str_ptr).to_str().unwrap(), c_str_ptr)
    }).collect();

    let query = make_str!(query, query_len);

    let mut match_results: Vec<_> = if query.trim().is_empty() {
        candidates.iter().map(|s| (s, 0)).collect()
    } else {
        candidates.iter()
            .filter_map(|s| matcher.fuzzy_match(s.0, query).map(|score| (s, score)))
            .collect()
    };

    fn count_word_occurrances(s: &str) -> usize {
        s.chars().filter(|&c| c == '_' || c == '.').count()
    }
    match_results.sort_by(|a, b| {
        // Sort by the store first, then the number of components, then lexically
        b.1.cmp(&a.1)
            // Ordering by words splitting by _ or . then by length then by the string itself
            .then_with(|| count_word_occurrances(a.0 .0).cmp(&count_word_occurrances(b.0 .0)))
            // Order by length.
            .then_with(|| a.0 .0.cmp(&b.0 .0))
    });

    unsafe {
        let result_count = std::cmp::min(match_results.len(), max_results);
        *actual_results = result_count;
        for (index, result) in match_results.iter().enumerate().take(result_count) {
            *ranked_candidates.add(index) = result.0 .1;
            if !ranked_scores.is_null() {
                *ranked_scores.add(index) = result.1 as u64;
            }
        }
    }
}

#[cfg(test)]
mod tests {}
