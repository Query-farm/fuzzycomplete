#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>



extern "C" {

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
int32_t perform_matches(const char *const *candidate_pool,
                        const int32_t *candidate_bonus,
                        size_t candidate_pool_size,
                        const char *query,
                        size_t query_len,
                        size_t max_results,
                        size_t *ranked_candidates,
                        uint64_t *ranked_scores,
                        size_t *actual_results);

} // extern "C"
