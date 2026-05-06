#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_WITH_FIT_MODE_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_WITH_FIT_MODE_H

#include "allocator_dbg_helper.h"

class allocator_with_fit_mode : public allocator_dbg_helper {
   public:
    enum class fit_mode { first_fit, the_best_fit, the_worst_fit };

   public:
    inline virtual void set_fit_mode(fit_mode mode) = 0;
};

#endif  // MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_WITH_FIT_MODE_H