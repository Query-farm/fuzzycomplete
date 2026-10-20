#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>



extern "C" {

void perform_matches(const char *const *candidate_pool,
                     size_t candidate_pool_size,
                     const char *query,
                     size_t query_len,
                     size_t max_results,
                     const char **ranked_candidates,
                     uint64_t *ranked_scores,
                     size_t *actual_results);

} // extern "C"
