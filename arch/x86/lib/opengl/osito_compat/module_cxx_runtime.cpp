/* Minimal libstdc++ policy support needed by Mesa's ASTC lookup tables. */

#include <initializer_list>
#include <tuple>
#include <ext/aligned_buffer.h>
#include <ext/alloc_traits.h>
#include <bits/functional_hash.h>
#include <bits/hashtable_policy.h>

namespace std _GLIBCXX_VISIBILITY(default)
{
_GLIBCXX_BEGIN_NAMESPACE_VERSION
namespace __detail
{
static bool
is_prime(size_t value)
{
   if (value < 2)
      return false;
   if ((value & 1) == 0)
      return value == 2;
   for (size_t divisor = 3; divisor <= value / divisor; divisor += 2) {
      if (value % divisor == 0)
         return false;
   }
   return true;
}

size_t
_Prime_rehash_policy::_M_next_bkt(size_t requested) const
{
   if (requested == 0)
      return 1;

   size_t candidate = requested <= 2 ? 2 : (requested | 1);
   while (!is_prime(candidate)) {
      if (candidate > size_t(-1) - 2) {
         candidate = size_t(-1);
         break;
      }
      candidate += 2;
   }

   if (candidate == size_t(-1))
      _M_next_resize = size_t(-1);
   else
      _M_next_resize = __builtin_floor(candidate * (double)_M_max_load_factor);
   return candidate;
}

pair<bool, size_t>
_Prime_rehash_policy::_M_need_rehash(size_t bucket_count, size_t element_count,
                                    size_t insert_count) const
{
   if (element_count + insert_count <= _M_next_resize)
      return { false, 0 };

   size_t required = element_count + insert_count;
   if (_M_next_resize == 0 && required < 11)
      required = 11;
   double min_buckets = required / (double)_M_max_load_factor;
   if (min_buckets >= bucket_count) {
      size_t target = (size_t)__builtin_floor(min_buckets) + 1;
      size_t growth = bucket_count * _S_growth_factor;
      if (target < growth)
         target = growth;
      return { true, _M_next_bkt(target) };
   }

   _M_next_resize = __builtin_floor(bucket_count * (double)_M_max_load_factor);
   return { false, 0 };
}
} /* namespace __detail */
_GLIBCXX_END_NAMESPACE_VERSION
} /* namespace std */
