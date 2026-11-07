// duckdb_fuzzycomplete_rust
// Copyright 2024 Rusty Conover <rusty@conover.me>
// Licensed under the MIT License

use std::borrow::Cow;
use std::ffi::{c_char, CStr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::slice;

fn count_word_separators(s: &str) -> usize {
    s.chars().filter(|&c| c == '_' || c == '.').count()
}

/// Rank `candidates` (text, bonus) against `query` and return the indexes of the best
/// `max_results` matches together with their fuzzy match scores (higher is better).
///
/// Candidates that do not fuzzy match the query are dropped. Matches are ordered by fuzzy score,
/// then by bonus (higher first, so e.g. user tables come before system ones), then by the number
/// of `_`/`.` separated components, then lexically.
fn rank(candidates: &[(Cow<'_, str>, i32)], query: &str, max_results: usize) -> Vec<(usize, u64)> {
    let mut matcher = code_fuzzy_match::FuzzyMatcher::new();
    let mut matches: Vec<(usize, u64)> = if query.trim().is_empty() {
        (0..candidates.len()).map(|i| (i, 0)).collect()
    } else {
        candidates
            .iter()
            .enumerate()
            .filter_map(|(i, c)| matcher.fuzzy_match(&c.0, query).map(|score| (i, score as u64)))
            .collect()
    };

    matches.sort_by(|a, b| {
        let (ca, cb) = (&candidates[a.0], &candidates[b.0]);
        b.1.cmp(&a.1)
            .then_with(|| cb.1.cmp(&ca.1))
            .then_with(|| count_word_separators(&ca.0).cmp(&count_word_separators(&cb.0)))
            .then_with(|| ca.0.cmp(&cb.0))
    });
    matches.truncate(max_results);
    matches
}

/// Fuzzy match `query` against a pool of NUL-terminated candidate strings.
///
/// * `candidate_bonus` may be null; otherwise it holds one bonus per candidate (higher ranks earlier
///   among equally good matches).
/// * `ranked_candidates` receives indexes into the candidate pool, `ranked_scores` (may be null) the
///   fuzzy match score of each (higher is better). Both must have room for
///   `min(max_results, candidate_pool_size)` entries.
/// * Candidates that are not valid UTF-8 (e.g. file names on Linux) are matched lossily.
///
/// Returns 0 on success and -1 on an invalid argument or an internal panic (no results are produced
/// then). A panic never unwinds into the caller.
#[no_mangle]
pub extern "C" fn perform_matches(
    candidate_pool: *const *const c_char,
    candidate_bonus: *const i32,
    candidate_pool_size: usize,
    query: *const c_char,
    query_len: usize,
    max_results: usize,
    ranked_candidates: *mut usize,
    ranked_scores: *mut u64,
    actual_results: *mut usize,
) -> i32 {
    if actual_results.is_null() {
        return -1;
    }
    unsafe { *actual_results = 0 };
    if (candidate_pool.is_null() && candidate_pool_size > 0)
        || (query.is_null() && query_len > 0)
        || (ranked_candidates.is_null() && max_results > 0)
    {
        return -1;
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let pool: &[*const c_char] = if candidate_pool_size == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(candidate_pool, candidate_pool_size) }
        };
        let bonuses: Option<&[i32]> = if candidate_bonus.is_null() || candidate_pool_size == 0 {
            None
        } else {
            Some(unsafe { slice::from_raw_parts(candidate_bonus, candidate_pool_size) })
        };
        let candidates: Vec<(Cow<'_, str>, i32)> = pool
            .iter()
            .enumerate()
            .map(|(i, &p)| {
                let text = if p.is_null() {
                    Cow::Borrowed("")
                } else {
                    unsafe { CStr::from_ptr(p) }.to_string_lossy()
                };
                (text, bonuses.map_or(0, |b| b[i]))
            })
            .collect();
        let query_bytes: &[u8] = if query_len == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(query as *const u8, query_len) }
        };
        let query = String::from_utf8_lossy(query_bytes);
        rank(&candidates, &query, max_results)
    }));

    match result {
        Ok(matches) => unsafe {
            for (index, (candidate, score)) in matches.iter().enumerate() {
                *ranked_candidates.add(index) = *candidate;
                if !ranked_scores.is_null() {
                    *ranked_scores.add(index) = *score;
                }
            }
            *actual_results = matches.len();
            0
        },
        Err(_) => -1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn c(s: &str, bonus: i32) -> (Cow<'_, str>, i32) {
        (Cow::Borrowed(s), bonus)
    }

    #[test]
    fn bonus_breaks_ties() {
        let pool = [c("DROP ", 0), c("DELETE ", 1), c("DESCRIBE ", 0)];
        let ranked = rank(&pool, "D", 10);
        assert_eq!(ranked[0].0, 1);
    }

    #[test]
    fn non_matching_dropped_and_truncated() {
        let pool = [c("alpha", 0), c("beta", 0), c("alphabet", 0)];
        assert_eq!(rank(&pool, "alp", 1).len(), 1);
        assert!(rank(&pool, "zzz", 10).is_empty());
    }

    #[test]
    fn ffi_handles_invalid_utf8_and_nulls() {
        let bad = b"caf\xff.csv\0";
        let good = b"cafe.csv\0";
        let pool = [bad.as_ptr() as *const c_char, good.as_ptr() as *const c_char];
        let mut out = [0usize; 2];
        let mut scores = [0u64; 2];
        let mut n = 0usize;
        let q = b"caf";
        let rc = perform_matches(
            pool.as_ptr(),
            std::ptr::null(),
            2,
            q.as_ptr() as *const c_char,
            q.len(),
            2,
            out.as_mut_ptr(),
            scores.as_mut_ptr(),
            &mut n,
        );
        assert_eq!(rc, 0);
        assert_eq!(n, 2);
        let rc = perform_matches(
            std::ptr::null(),
            std::ptr::null(),
            0,
            std::ptr::null(),
            0,
            0,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut n,
        );
        assert_eq!(rc, 0);
        assert_eq!(n, 0);
    }
}
